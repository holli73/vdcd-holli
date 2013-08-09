//
//  devicecontainer.cpp
//  p44bridged
//
//  Created by Lukas Zeller on 17.04.13.
//  Copyright (c) 2013 plan44.ch. All rights reserved.
//

#include "devicecontainer.hpp"

#include "deviceclasscontainer.hpp"

#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <netinet/in.h>
#include <string.h>

#include "device.hpp"

#include "fnv.hpp"

// for local behaviour
#include "buttonbehaviour.hpp"
#include "lightbehaviour.hpp"


using namespace p44;


DeviceContainer::DeviceContainer() :
  vdcApiServer(SyncIOMainLoop::currentMainLoop()),
  collecting(false),
  announcementTicket(0),
  periodicTaskTicket(0),
  localDimTicket(0),
  localDimDown(false)
{
  #warning "// TODO: %%%% use final dsid scheme"
  // create a hash of the deviceContainerInstanceIdentifier
  string s = deviceContainerInstanceIdentifier();
  Fnv64 hash;
  hash.addBytes(s.size(), (uint8_t *)s.c_str());
  #if FAKE_REAL_DSD_IDS
  containerDsid.setObjectClass(DSID_OBJECTCLASS_DSDEVICE);
  containerDsid.setSerialNo(hash.getHash32());
  #warning "TEST ONLY: faking digitalSTROM device addresses, possibly colliding with real devices"
  #else
  // TODO: validate, now we are using the MAC-address class with bits 48..51 set to 7
  containerDsid.setObjectClass(DSID_OBJECTCLASS_MACADDRESS);
  containerDsid.setSerialNo(0x7000000000000ll+hash.getHash48());
  #endif
}


void DeviceContainer::addDeviceClassContainer(DeviceClassContainerPtr aDeviceClassContainerPtr)
{
  deviceClassContainers.push_back(aDeviceClassContainerPtr);
  aDeviceClassContainerPtr->setDeviceContainer(this);
}



string DeviceContainer::deviceContainerInstanceIdentifier() const
{
  string identifier;

  struct ifreq ifr;
  struct ifconf ifc;
  char buf[1024];
  int success = 0;

  do {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) { /* handle error*/ };

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1)
      break;
    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));
    for (; it != end; ++it) {
      strcpy(ifr.ifr_name, it->ifr_name);
      if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        if (! (ifr.ifr_flags & IFF_LOOPBACK)) { // don't count loopback
          #ifdef __APPLE__
          #warning MAC address retrieval on OSX not supported
          break;
          #else
          if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
            success = 1;
            break;
          }
          #endif
        }
      }
      else
        break;
    }
  } while(false);
  // extract ID if we have one
  if (success) {
    for (int i=0; i<6; ++i) {
      #ifndef __APPLE__
      string_format_append(identifier, "%02X",(uint8_t *)(ifr.ifr_hwaddr.sa_data)[i]);
      #endif
    }
  }
  else {
    identifier = "UnknownMACAddress";
  }
  return identifier;
}


void DeviceContainer::setPersistentDataDir(const char *aPersistentDataDir)
{
	persistentDataDir = nonNullCStr(aPersistentDataDir);
	if (!persistentDataDir.empty() && persistentDataDir[persistentDataDir.length()-1]!='/') {
		persistentDataDir.append("/");
	}
}


const char *DeviceContainer::getPersistentDataDir()
{
	return persistentDataDir.c_str();
}





#pragma mark - initializisation of DB and containers


class DeviceClassInitializer
{
  CompletedCB callback;
  list<DeviceClassContainerPtr>::iterator nextContainer;
  DeviceContainer *deviceContainerP;
  bool factoryReset;
public:
  static void initialize(DeviceContainer *aDeviceContainerP, CompletedCB aCallback, bool aFactoryReset)
  {
    // create new instance, deletes itself when finished
    new DeviceClassInitializer(aDeviceContainerP, aCallback, aFactoryReset);
  };
private:
  DeviceClassInitializer(DeviceContainer *aDeviceContainerP, CompletedCB aCallback, bool aFactoryReset) :
		callback(aCallback),
		deviceContainerP(aDeviceContainerP),
    factoryReset(aFactoryReset)
  {
    nextContainer = deviceContainerP->deviceClassContainers.begin();
    queryNextContainer(ErrorPtr());
  }


  void queryNextContainer(ErrorPtr aError)
  {
    if ((!aError || factoryReset) && nextContainer!=deviceContainerP->deviceClassContainers.end())
      (*nextContainer)->initialize(boost::bind(&DeviceClassInitializer::containerInitialized, this, _1), factoryReset);
    else
      completed(aError);
  }

  void containerInitialized(ErrorPtr aError)
  {
    // check next
    ++nextContainer;
    queryNextContainer(aError);
  }

  void completed(ErrorPtr aError)
  {
    // start periodic tasks like registration checking and saving parameters
    MainLoop::currentMainLoop()->executeOnce(boost::bind(&DeviceContainer::periodicTask, deviceContainerP, _2), 1*Second, deviceContainerP);
    // callback
    callback(aError);
    // done, delete myself
    delete this;
  }

};


#define DSPARAMS_SCHEMA_VERSION 1

string DsParamStore::dbSchemaUpgradeSQL(int aFromVersion, int &aToVersion)
{
  string sql;
  if (aFromVersion==0) {
    // create DB from scratch
		// - use standard globs table for schema version
    sql = inherited::dbSchemaUpgradeSQL(aFromVersion, aToVersion);
		// - no devicecontainer level table to create at this time
    //   (PersistentParams create and update their tables as needed)
    // reached final version in one step
    aToVersion = DSPARAMS_SCHEMA_VERSION;
  }
  return sql;
}


void DeviceContainer::initialize(CompletedCB aCompletedCB, bool aFactoryReset)
{
  // start the API server
  vdcApiServer.startServer(boost::bind(&DeviceContainer::vdcApiConnectionHandler, this, _1), 3);
  // initialize dsParamsDB database
	string databaseName = getPersistentDataDir();
	string_format_append(databaseName, "DsParams.sqlite3");
  ErrorPtr error = dsParamStore.connectAndInitialize(databaseName.c_str(), DSPARAMS_SCHEMA_VERSION, aFactoryReset);

  // start initialisation of class containers
  DeviceClassInitializer::initialize(this, aCompletedCB, aFactoryReset);
}



SocketCommPtr DeviceContainer::vdcApiConnectionHandler(SocketComm *aServerSocketCommP)
{
  JsonRpcCommPtr conn = JsonRpcCommPtr(new JsonRpcComm(SyncIOMainLoop::currentMainLoop()));
  conn->setRequestHandler(boost::bind(&DeviceContainer::vdcApiRequestHandler, this, _1, _2, _3, _4));
  return conn;
}


void DeviceContainer::vdcApiRequestHandler(JsonRpcComm *aJsonRpcComm, const char *aMethod, const char *aJsonRpcId, JsonObjectPtr aParams)
{
  LOG(LOG_DEBUG,"vDC API request id='%s', method='%s', params=%s\n", aJsonRpcId, aMethod, aParams ? aParams->c_strValue() : "<none>");
  aJsonRpcComm->sendError(aJsonRpcId, JSONRPC_METHOD_NOT_FOUND, "API not yet implemented");
}





#pragma mark - collect devices

namespace p44 {

/// collects and initializes all devices
class DeviceClassCollector
{
  CompletedCB callback;
  bool exhaustive;
  list<DeviceClassContainerPtr>::iterator nextContainer;
  DeviceContainer *deviceContainerP;
  DsDeviceMap::iterator nextDevice;
public:
  static void collectDevices(DeviceContainer *aDeviceContainerP, CompletedCB aCallback, bool aExhaustive)
  {
    // create new instance, deletes itself when finished
    new DeviceClassCollector(aDeviceContainerP, aCallback, aExhaustive);
  };
private:
  DeviceClassCollector(DeviceContainer *aDeviceContainerP, CompletedCB aCallback, bool aExhaustive) :
    callback(aCallback),
    deviceContainerP(aDeviceContainerP),
    exhaustive(aExhaustive)
  {
    nextContainer = deviceContainerP->deviceClassContainers.begin();
    queryNextContainer(ErrorPtr());
  }


  void queryNextContainer(ErrorPtr aError)
  {
    if (!aError && nextContainer!=deviceContainerP->deviceClassContainers.end())
      (*nextContainer)->collectDevices(boost::bind(&DeviceClassCollector::containerQueried, this, _1), exhaustive);
    else
      collectedAll(aError);
  }

  void containerQueried(ErrorPtr aError)
  {
    // check next
    ++nextContainer;
    queryNextContainer(aError);
  }


  void collectedAll(ErrorPtr aError)
  {
    // now have each of them initialized
    nextDevice = deviceContainerP->dSDevices.begin();
    initializeNextDevice(ErrorPtr());
  }


  void initializeNextDevice(ErrorPtr aError)
  {
    if (!aError && nextDevice!=deviceContainerP->dSDevices.end())
      // TODO: now never doing factory reset init, maybe parametrize later
      nextDevice->second->initializeDevice(boost::bind(&DeviceClassCollector::deviceInitialized, this, _1), false);
    else
      completed(aError);
  }


  void deviceInitialized(ErrorPtr aError)
  {
    // check next
    ++nextDevice;
    initializeNextDevice(aError);
  }


  void completed(ErrorPtr aError)
  {
    callback(aError);
    deviceContainerP->collecting = false;
    // done, delete myself
    delete this;
  }

};



void DeviceContainer::collectDevices(CompletedCB aCompletedCB, bool aExhaustive)
{
  if (!collecting) {
    collecting = true;
    dSDevices.clear(); // forget existing ones
    DeviceClassCollector::collectDevices(this, aCompletedCB, aExhaustive);
  }
}

} // namespace


#pragma mark - adding/removing devices


// add a new device, replaces possibly existing one based on dsid
void DeviceContainer::addDevice(DevicePtr aDevice)
{
  // set for given dsid in the container-wide map of devices
  dSDevices[aDevice->dsid] = aDevice;
  LOG(LOG_NOTICE,"--- added device: %s", aDevice->description().c_str());
  // load the device's persistent params
  aDevice->load();
  // unless collecting now, register new device right away
  if (!collecting) {
    announceDevices();
  }
}


// remove a device
void DeviceContainer::removeDevice(DevicePtr aDevice, bool aForget)
{
  if (aForget) {
    // permanently remove from DB
    aDevice->forget();
  }
  else {
    // save, as we don't want to forget the settings associated with the device
    aDevice->save();
  }
  // send vanish message
  JsonObjectPtr params = JsonObject::newObj();
  params->add("dSidentifier", JsonObject::newString(aDevice->dsid.getString()));
  sendMessage("Vanish", params);
  // remove from container-wide map of devices
  dSDevices.erase(aDevice->dsid);
  LOG(LOG_NOTICE,"--- removed device: %s", aDevice->description().c_str());
  // TODO: maybe unregister from vdSM???
}



#pragma mark - periodic activity


#define PERIODIC_TASK_INTERVAL (3*Second)

void DeviceContainer::periodicTask(MLMicroSeconds aCycleStartTime)
{
  // cancel any pending executions
  MainLoop::currentMainLoop()->cancelExecutionTicket(periodicTaskTicket);
  if (!collecting) {
    // check for devices that need registration
    announceDevices();
    // do a save run as well
    for (DsDeviceMap::iterator pos = dSDevices.begin(); pos!=dSDevices.end(); ++pos) {
      pos->second->save();
    }
  }
  // schedule next run
  periodicTaskTicket = MainLoop::currentMainLoop()->executeOnce(boost::bind(&DeviceContainer::periodicTask, this, _2), PERIODIC_TASK_INTERVAL, this);
}



#pragma mark - message dispatcher

void DeviceContainer::vdsmMessageHandler(ErrorPtr aError, JsonObjectPtr aJsonObject)
{
  LOG(LOG_DEBUG, "Received vdSM API message: %s\n", aJsonObject->json_c_str());
  ErrorPtr err;
  JsonObjectPtr opObj = aJsonObject->get("operation");
  if (!opObj) {
    // no operation
    err = ErrorPtr(new vdSMError(vdSMErrorMissingOperation, "missing 'operation'"));
  }
  else {
    // get operation as lowercase string (to make comparisons case insensitive, we do all in lowercase)
    string o = opObj->lowercaseStringValue();
    // check for parameter addressing a device
    DevicePtr dev;
    JsonObjectPtr paramsObj = aJsonObject->get("parameter");
    bool doesAddressDevice = false;
    if (paramsObj) {
      // first check for dSID
      JsonObjectPtr dsidObj = paramsObj->get("dSidentifier");
      if (dsidObj) {
        string s = dsidObj->stringValue();
        dSID dsid(s);
        doesAddressDevice = true;
        DsDeviceMap::iterator pos = dSDevices.find(dsid);
        if (pos!=dSDevices.end())
          dev = pos->second;
      }
    }
    // dev now set to target device if one could be found
    if (dev) {
      // check operations targeting a device
      if (o=="deviceregistrationack") {
        dev->announcementAck(paramsObj);
        // signal device registered, so next can be issued
        deviceAnnounced();
      }
      else {
        // just forward message to device
        err = dev->handleMessage(o, paramsObj);
      }
    }
    else {
      // could not find a device to send the message to
      if (doesAddressDevice) {
        // if the message was actually targeting a device, this is an error
        err = ErrorPtr(new vdSMError(vdSMErrorDeviceNotFound, "device not found"));
      }
      else {
        // check operations not targeting a device
        // TODO: add operations
        // unknown operation
        err = ErrorPtr(new vdSMError(vdSMErrorUnknownContainerOperation, string_format("unknown container operation '%s'", o.c_str())));
      }
    }
  }
  if (!Error::isOK(err)) {
    LOG(LOG_ERR, "vdSM message processing error: %s\n", err->description().c_str());
    // send back error response
    JsonObjectPtr params = JsonObject::newObj();
    params->add("dSidentifier", JsonObject::newString(containerDsid.getString()));
    params->add("Message", JsonObject::newString(err->description()));
    sendMessage("Error", params);
  }
}


void DeviceContainer::localDimHandler()
{
  for (DsDeviceMap::iterator pos = dSDevices.begin(); pos!=dSDevices.end(); ++pos) {
    DevicePtr dev = pos->second;
    LightBehaviour *lightBehaviour = dynamic_cast<LightBehaviour *>(dev->behaviourP);
    if (lightBehaviour) {
      lightBehaviour->callScene(localDimDown ? DEC_S : INC_S);
    }
  }
  localDimTicket = MainLoop::currentMainLoop()->executeOnce(boost::bind(&DeviceContainer::localDimHandler, this), 250*MilliSecond, this);
}


void DeviceContainer::handleClickLocally(int aClickType, int aKeyID)
{
  // TODO: Not really conforming to ds-light yet...
  int scene = -1; // none
  int direction = aKeyID==ButtonBehaviour::key_2way_A ? 1 : (aKeyID==ButtonBehaviour::key_2way_B ? -1 : 0); // -1=down/off, 1=up/on, 0=toggle
  switch (aClickType) {
    case ButtonBehaviour::ct_tip_1x:
      scene = T0_S1;
      break;
    case ButtonBehaviour::ct_tip_2x:
      scene = T0_S2;
      break;
    case ButtonBehaviour::ct_tip_3x:
      scene = T0_S3;
      break;
    case ButtonBehaviour::ct_tip_4x:
      scene = T0_S4;
      break;
    case ButtonBehaviour::ct_hold_start:
      scene = direction>0 ? MIN_S : INC_S;
      localDimTicket = MainLoop::currentMainLoop()->executeOnce(boost::bind(&DeviceContainer::localDimHandler, this), 250*MilliSecond, this);
      if (direction!=0)
        localDimDown = direction<0;
      else {
        localDimDown = !localDimDown; // just toggle direction
        direction = localDimDown ? -1 : 1; // adjust direction as well
      }
      break;
    case ButtonBehaviour::ct_hold_end:
      MainLoop::currentMainLoop()->cancelExecutionTicket(localDimTicket); // stop dimming
      scene = STOP_S; // stop any still ongoing dimming
      direction = 1; // really send STOP, not main off!
      break;
  }
  if (scene>=0) {
    for (DsDeviceMap::iterator pos = dSDevices.begin(); pos!=dSDevices.end(); ++pos) {
      DevicePtr dev = pos->second;
      LightBehaviour *lightBehaviour = dynamic_cast<LightBehaviour *>(dev->behaviourP);
      if (lightBehaviour) {
        // this is a light
        if (direction==0) {
          // get direction from current value of first encountered light
          direction = lightBehaviour->getLogicalBrightness()>0 ? -1 : 1;
        }
        // determine the scene to call
        int effScene = scene;
        if (scene==INC_S) {
          // dimming
          if (direction<0) effScene = DEC_S;
        }
        else {
          // switching
          if (direction<0) effScene = T0_S0; // main off
        }
        // call the effective scene
        lightBehaviour->callScene(effScene);
      }
    }
  }
}


bool DeviceContainer::sendMessage(const char *aOperation, JsonObjectPtr aParams)
{
  // TODO: %%% cleaner implementation for this, q&d hack for now only
  /*
  if (!vdsmJsonComm.connectable()) {
    // not connectable, check some messages to interpret locally for standalone mode
    if (strcmp(aOperation,"DeviceButtonClick")==0) {
      // handle button clicks locally
      int c = aParams->get("click")->int32Value();
      int k = aParams->get("key")->int32Value();
      handleClickLocally(c, k);
    }
    // not really sent
    return false;
  }
  JsonObjectPtr req = JsonObject::newObj();
  req->add("operation", JsonObject::newString(aOperation));
  if (aParams) {
    req->add("parameter", aParams);
  }
  ErrorPtr err = vdsmJsonComm.sendMessage(req);
  LOG(LOG_DEBUG, "Sent vdSM API message: %s\n", req->json_c_str());
  if (!Error::isOK(err)) {
    LOG(LOG_INFO, "Error sending JSON message: %s\n", err->description().c_str());
    return false;
  }
  */
  return true;
}


#pragma mark - session management


/// start vDC session (say Hello to the vdSM)
void DeviceContainer::startContainerSession()
{
  // end previous container session first (set all devices unannounced)
  endContainerSession();
  // send Hello
  JsonObjectPtr params = JsonObject::newObj();
  params->add("dSidentifier", JsonObject::newString(containerDsid.getString()));
  params->add("APIVersion", JsonObject::newInt32(1)); // TODO: %%% luz: must be 1=aizo, dsa cannot expand other ids so far
  sendMessage("Hello", params);
  #warning "For now, vdSM does not understand Hello, so we are not waiting for an answer yet"
  // %%% continue with announcing devices
  announceDevices();
}


/// end vDC session
void DeviceContainer::endContainerSession()
{
  // end pending announcement
  MainLoop::currentMainLoop()->cancelExecutionTicket(announcementTicket);
  // end all device sessions
  for (DsDeviceMap::iterator pos = dSDevices.begin(); pos!=dSDevices.end(); ++pos) {
    DevicePtr dev = pos->second;
    dev->announced = Never;
    dev->announcing = Never;
  }
}


// how long until a not acknowledged registrations is considered timed out (and next device can be attempted)
#define REGISTRATION_TIMEOUT (15*Second)

// how long until a not acknowledged announcement for a device is retried again for the same device
#define REGISTRATION_RETRY_TIMEOUT (300*Second)

/// announce all not-yet announced devices to the vdSM
void DeviceContainer::announceDevices()
{
  /*
  if (!collecting && announcementTicket==0 && vdsmJsonComm.connected()) {
    // check all devices for unnannounced ones and announce those
    for (DsDeviceMap::iterator pos = dSDevices.begin(); pos!=dSDevices.end(); ++pos) {
      DevicePtr dev = pos->second;
      if (
        dev->isPublicDS() && // only public ones
        dev->announced==Never &&
        (dev->announcing==Never || MainLoop::now()>dev->announcing+REGISTRATION_RETRY_TIMEOUT)
      ) {
        // mark device as being in process of getting registered
        dev->announcing = MainLoop::now();
        // send registration request
        #warning "// TODO: for new vDC API, replace this by "Announce" method
        if (!sendMessage("DeviceRegistration", dev->registrationParams())) {
          LOG(LOG_ERR, "Could not send announcement message for device %s\n", dev->shortDesc().c_str());
          dev->announcing = Never; // not registering
        }
        else {
          LOG(LOG_INFO, "Sent announcement for device %s\n", dev->shortDesc().c_str());
        }
        // don't register too fast, prevent re-registering for a while
        announcementTicket = MainLoop::currentMainLoop()->executeOnce(boost::bind(&DeviceContainer::deviceAnnounced, this), REGISTRATION_TIMEOUT);
        // done for now, continues after REGISTRATION_TIMEOUT or when registration acknowledged
        break;
      }
    }
  }
  */
}


void DeviceContainer::deviceAnnounced()
{
  MainLoop::currentMainLoop()->cancelExecutionTicket(announcementTicket);
  announcementTicket = 0;
  // try next announcement
  announceDevices();
}


#pragma mark - description

string DeviceContainer::description()
{
  string d = string_format("DeviceContainer with %d device classes:\n", deviceClassContainers.size());
  for (ContainerList::iterator pos = deviceClassContainers.begin(); pos!=deviceClassContainers.end(); ++pos) {
    d.append((*pos)->description());
  }
  return d;
}


