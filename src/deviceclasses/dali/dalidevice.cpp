//
//  Copyright (c) 2013-2014 plan44.ch / Lukas Zeller, Zurich, Switzerland
//
//  Author: Lukas Zeller <luz@plan44.ch>
//
//  This file is part of vdcd.
//
//  vdcd is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  vdcd is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with vdcd. If not, see <http://www.gnu.org/licenses/>.
//


// File scope debugging options
// - Set ALWAYS_DEBUG to 1 to enable DBGLOG output even in non-DEBUG builds of this file
#define ALWAYS_DEBUG 0
// - set FOCUSLOGLEVEL to non-zero log level (usually, 5,6, or 7==LOG_DEBUG) to get focus (extensive logging) for this file
//   Note: must be before including "logger.hpp" (or anything that includes "logger.hpp")
#define FOCUSLOGLEVEL 0


#include "dalidevice.hpp"
#include "dalidevicecontainer.hpp"

#include "fnv.hpp"

#include "colorlightbehaviour.hpp"

#include <math.h>

using namespace p44;


#pragma mark - DaliBusDevice

DaliBusDevice::DaliBusDevice(DaliDeviceContainer &aDaliDeviceContainer) :
  daliDeviceContainer(aDaliDeviceContainer),
  dimRepeaterTicket(0),
  isDummy(false),
  isPresent(false),
  lampFailure(false),
  currentTransitionTime(Infinite), // invalid
  currentDimPerMS(0), // none
  currentFadeRate(0xFF), currentFadeTime(0xFF) // unlikely values
{
}



void DaliBusDevice::setDeviceInfo(DaliDeviceInfo aDeviceInfo)
{
  // store the info record
  deviceInfo = aDeviceInfo; // copy
  derivedDsUid(); // derive dSUID from it
}


void DaliBusDevice::derivedDsUid()
{
  if (isDummy) return;
  // vDC implementation specific UUID:
  DsUid vdcNamespace(DSUID_P44VDC_NAMESPACE_UUID);
  string s;
  #ifdef OLD_BUGGY_CHKSUM_COMPATIBLE
  if (deviceInfo.devInfStatus==DaliDeviceInfo::devinf_maybe) {
    // assume we can use devInf to derive dSUID from
    deviceInfo.devInfStatus = DaliDeviceInfo::devinf_solid;
    // but only actually use it if there is no device entry for the shortaddress-based dSUID with a non-zero name
    // (as this means the device has been already actively used/configured with the shortaddr-dSUID)
    // - calculate the short address based dSUID
    s = daliDeviceContainer.deviceClassContainerInstanceIdentifier();
    string_format_append(s, "::%d", deviceInfo.shortAddress);
    DsUid shortAddrBasedDsUid;
    shortAddrBasedDsUid.setNameInSpace(s, vdcNamespace);
    // - check for named device in database consisting of this dimmer with shortaddr based dSUID
    //   Note that only single dimmer device are checked for, composite devices will not have this compatibility mechanism
    sqlite3pp::query qry(daliDeviceContainer.getDeviceContainer().getDsParamStore());
    // Note: this is a bit ugly, as it has the device settings table name hard coded
    string sql = string_format("SELECT deviceName FROM DeviceSettings WHERE parentID='%s'", shortAddrBasedDsUid.getString().c_str());
    if (qry.prepare(sql.c_str())==SQLITE_OK) {
      sqlite3pp::query::iterator i = qry.begin();
      if (i!=qry.end()) {
        // the length of the name
        string n = nonNullCStr(i->get<const char *>(0));
        if (n.length()>0) {
          // shortAddr based device has already been named. So keep that, and don't generate a devInf based dSUID
          deviceInfo.devInfStatus = DaliDeviceInfo::devinf_notForID;
          LOG(LOG_WARNING, "DaliBusDevice shortaddr %d kept with shortaddr-based dSUID because it is already named: '%s'\n", deviceInfo.shortAddress, n.c_str());
        }
      }
    }
  }
  #endif // OLD_BUGGY_CHKSUM_COMPATIBLE
  if (deviceInfo.devInfStatus==DaliDeviceInfo::devinf_solid) {
    // uniquely identified by GTIN+Serial, but unknown partition value:
    // - Proceed according to dS rule 2:
    //   "vDC can determine GTIN and serial number of Device → combine GTIN and
    //    serial number to form a GS1-128 with Application Identifier 21:
    //    "(01)<GTIN>(21)<serial number>" and use the resulting string to
    //    generate a UUIDv5 in the GS1-128 name space"
    s = string_format("(01)%llu(21)%llu", deviceInfo.gtin, deviceInfo.serialNo);
  }
  else {
    // not uniquely identified by devInf (or shortaddr based version already in use):
    // - generate id in vDC namespace
    //   UUIDv5 with name = classcontainerinstanceid::daliShortAddrDecimal
    s = daliDeviceContainer.deviceClassContainerInstanceIdentifier();
    string_format_append(s, "::%d", deviceInfo.shortAddress);
  }
  dSUID.setNameInSpace(s, vdcNamespace);
}


void DaliBusDevice::updateParams(CompletedCB aCompletedCB)
{
  if (isDummy) aCompletedCB(ErrorPtr());
  // query actual arc power level
  daliDeviceContainer.daliComm->daliSendQuery(
    deviceInfo.shortAddress,
    DALICMD_QUERY_ACTUAL_LEVEL,
    boost::bind(&DaliBusDevice::queryActualLevelResponse,this, aCompletedCB, _1, _2, _3)
  );
}


void DaliBusDevice::queryActualLevelResponse(CompletedCB aCompletedCB, bool aNoOrTimeout, uint8_t aResponse, ErrorPtr aError)
{
  currentBrightness = 0; // default to 0
  if (Error::isOK(aError) && !aNoOrTimeout) {
    isPresent = true; // answering a query means presence
    // this is my current arc power, save it as brightness for dS system side queries
    currentBrightness = arcpowerToBrightness(aResponse);
    LOG(LOG_DEBUG, "DaliBusDevice: retrieved current dimming level: arc power = %d, brightness = %0.1f\n", aResponse, currentBrightness);
  }
  // next: query the minimum dimming level
  daliDeviceContainer.daliComm->daliSendQuery(
    deviceInfo.shortAddress,
    DALICMD_QUERY_MIN_LEVEL,
    boost::bind(&DaliBusDevice::queryMinLevelResponse,this, aCompletedCB, _1, _2, _3)
  );
}


void DaliBusDevice::queryMinLevelResponse(CompletedCB aCompletedCB, bool aNoOrTimeout, uint8_t aResponse, ErrorPtr aError)
{
  minBrightness = 0; // default to 0
  if (Error::isOK(aError) && !aNoOrTimeout) {
    isPresent = true; // answering a query means presence
    // this is my current arc power, save it as brightness for dS system side queries
    minBrightness = arcpowerToBrightness(aResponse);
    LOG(LOG_DEBUG, "DaliBusDevice: retrieved minimum dimming level: arc power = %d, brightness = %0.1f\n", aResponse, minBrightness);
  }
  // done updating parameters
  aCompletedCB(aError);
}


void DaliBusDevice::updateStatus(CompletedCB aCompletedCB)
{
  if (isDummy) aCompletedCB(ErrorPtr());
  // query the device for status
  daliDeviceContainer.daliComm->daliSendQuery(
    deviceInfo.shortAddress, DALICMD_QUERY_STATUS,
    boost::bind(&DaliBusDevice::queryStatusResponse, this, aCompletedCB, _1, _2, _3)
  );
}


void DaliBusDevice::queryStatusResponse(CompletedCB aCompletedCB, bool aNoOrTimeout, uint8_t aResponse, ErrorPtr aError)
{
  if (Error::isOK(aError) && !aNoOrTimeout) {
    isPresent = true; // answering a query means presence
    // check status bits
    // - bit1 = lamp failure
    lampFailure = aResponse & 0x02;
  }
  else {
    isPresent = false; // no correct status -> not present
  }
  // done updating status
  aCompletedCB(aError);
}



void DaliBusDevice::setTransitionTime(MLMicroSeconds aTransitionTime)
{
  if (isDummy) return;
  if (currentTransitionTime==Infinite || currentTransitionTime!=aTransitionTime) {
    uint8_t tr = 0; // default to 0
    if (aTransitionTime>0) {
      // Fade time: T = 0.5 * SQRT(2^X) [seconds] -> x = ln2((T/0.5)^2) : T=0.25 [sec] -> x = -2, T=10 -> 8.64
      double h = (((double)aTransitionTime/Second)/0.5);
      h = h*h;
      h = log(h)/log(2);
      tr = h>1 ? (uint8_t)h : 1;
      LOG(LOG_DEBUG, "DaliDevice: new transition time = %.1f mS, calculated FADE_TIME setting = %f (rounded %d)\n", (double)aTransitionTime/MilliSecond, h, (int)tr);
    }
    if (tr!=currentFadeTime || currentTransitionTime==Infinite) {
      LOG(LOG_DEBUG, "DaliDevice: setting DALI FADE_TIME to %d\n", (int)tr);
      daliDeviceContainer.daliComm->daliSendDtrAndConfigCommand(deviceInfo.shortAddress, DALICMD_STORE_DTR_AS_FADE_TIME, tr);
      currentFadeTime = tr;
    }
    currentTransitionTime = aTransitionTime;
  }
}


void DaliBusDevice::setBrightness(Brightness aBrightness)
{
  if (isDummy) return;
  if (currentBrightness!=aBrightness) {
    currentBrightness = aBrightness;
    uint8_t power = brightnessToArcpower(aBrightness);
    LOG(LOG_INFO, "Dali dimmer at shortaddr=%d: setting new brightness = %0.2f, arc power = %d\n", (int)deviceInfo.shortAddress, aBrightness, (int)power);
    daliDeviceContainer.daliComm->daliSendDirectPower(deviceInfo.shortAddress, power);
  }
}


uint8_t DaliBusDevice::brightnessToArcpower(Brightness aBrightness)
{
  double intensity = (double)aBrightness/100;
  if (intensity<0) intensity = 0;
  if (intensity>1) intensity = 1;
  return log10((intensity*9)+1)*254; // 0..254, 255 is MASK and is reserved to stop fading
}



Brightness DaliBusDevice::arcpowerToBrightness(int aArcpower)
{
  double intensity = (pow(10, aArcpower/254.0)-1)/9;
  return intensity*100;
}


// optimized DALI dimming implementation
void DaliBusDevice::dim(DsDimMode aDimMode, double aDimPerMS)
{
  if (isDummy) return;
  // start dimming
  FOCUSLOG("DALI dimmer %s\n", aDimMode==dimmode_stop ? "STOPS dimming" : (aDimMode==dimmode_up ? "starts dimming UP" : "starts dimming DOWN"));
  MainLoop::currentMainLoop().cancelExecutionTicket(dimRepeaterTicket); // stop any previous dimming activity
  // Use DALI UP/DOWN dimming commands
  if (aDimMode==dimmode_stop) {
    // stop dimming - send MASK
    daliDeviceContainer.daliComm->daliSendDirectPower(deviceInfo.shortAddress, DALIVALUE_MASK);
  }
  else {
    // start dimming
    // - configure new fade rate if current does not match
    if (aDimPerMS!=currentDimPerMS) {
      currentDimPerMS = aDimPerMS;
      //   Fade rate: R = 506/SQRT(2^X) [steps/second] -> x = ln2((506/R)^2) : R=44 [steps/sec] -> x = 7
      double h = 506.0/(currentDimPerMS*1000);
      h = log(h*h)/log(2);
      uint8_t fr = h>0 ? (uint8_t)h : 0;
      LOG(LOG_DEBUG, "DaliDevice: new dimming rate = %f Steps/second, calculated FADE_RATE setting = %f (rounded %d)\n", currentDimPerMS*1000, h, fr);
      if (fr!=currentFadeRate) {
        LOG(LOG_DEBUG, "DaliDevice: setting DALI FADE_RATE to %d\n", fr);
        daliDeviceContainer.daliComm->daliSendDtrAndConfigCommand(deviceInfo.shortAddress, DALICMD_STORE_DTR_AS_FADE_RATE, fr);
        currentFadeRate = fr;
      }
    }
    // - use repeated UP and DOWN commands
    dimRepeater(deviceInfo.shortAddress, aDimMode==dimmode_up ? DALICMD_UP : DALICMD_DOWN, MainLoop::now());
  }
}


void DaliBusDevice::dimRepeater(DaliAddress aDaliAddress, uint8_t aCommand, MLMicroSeconds aCycleStartTime)
{
  daliDeviceContainer.daliComm->daliSendCommand(aDaliAddress, aCommand);
  // schedule next command
  // - DALI UP and DOWN run 200mS, but can be repeated earlier, so we use 150mS to make sure we don't have hickups
  //   Note: DALI bus speed limits commands to 120Bytes/sec max, i.e. about 20 per 150mS, i.e. max 10 lamps dimming
  dimRepeaterTicket = MainLoop::currentMainLoop().executeOnceAt(boost::bind(&DaliBusDevice::dimRepeater, this, aDaliAddress, aCommand, _1), aCycleStartTime+200*MilliSecond);
}



#pragma mark - DaliDevice (single channel)


DaliDevice::DaliDevice(DaliDeviceContainer *aClassContainerP) :
  Device((DeviceClassContainer *)aClassContainerP)
{
  // DALI devices are always light (in this implementation, at least)
  setPrimaryGroup(group_yellow_light);
}

void DaliDevice::willBeAdded()
{
  // Note: setting up behaviours late, because we want the brightness dimmer already assigned for the hardware name
  // set up dS behaviour for simple single DALI channel dimmer
  // - use light settings, which include a scene table
  installSettings(DeviceSettingsPtr(new LightDeviceSettings(*this)));
  // - set the behaviour
  LightBehaviourPtr l = LightBehaviourPtr(new LightBehaviour(*this));
  l->setHardwareOutputConfig(outputFunction_dimmer, usage_undefined, true, 160); // DALI ballasts are always dimmable, // TODO: %%% somewhat arbitrary 2*80W max wattage
  l->setHardwareName(string_format("DALI dimmer @ %d",brightnessDimmer->deviceInfo.shortAddress));
  addBehaviour(l);
  // - derive the DsUid
  deriveDsUid();
}


DaliDeviceContainer &DaliDevice::daliDeviceContainer()
{
  return *(static_cast<DaliDeviceContainer *>(classContainerP));
}



bool DaliDevice::getDeviceIcon(string &aIcon, bool aWithData, const char *aResolutionPrefix)
{
  if (getIcon("dali_dimmer", aIcon, aWithData, aResolutionPrefix))
    return true;
  else
    return inherited::getDeviceIcon(aIcon, aWithData, aResolutionPrefix);
}


string DaliDevice::getExtraInfo()
{
  return string_format(
    "DALI short address: %d",
    brightnessDimmer->deviceInfo.shortAddress
  );
}


void DaliDevice::initializeDevice(CompletedCB aCompletedCB, bool aFactoryReset)
{
  // - sync cached channel values from actual device
  brightnessDimmer->updateParams(boost::bind(&DaliDevice::brightnessDimmerSynced, this, aCompletedCB, aFactoryReset, _1));
}


void DaliDevice::brightnessDimmerSynced(CompletedCB aCompletedCB, bool aFactoryReset, ErrorPtr aError)
{
  if (Error::isOK(aError)) {
    // save brightness now
    output->getChannelByIndex(0)->syncChannelValue(brightnessDimmer->currentBrightness);
    // initialize the light behaviour with the minimal dimming level
    LightBehaviourPtr l = boost::static_pointer_cast<LightBehaviour>(output);
    l->initMinBrightness(brightnessDimmer->minBrightness);
  }
  else {
    LOG(LOG_ERR, "DaliDevice: error getting state/params from dimmer: %s\n", aError->description().c_str());
  }
  // continue with initialisation in superclasses
  inherited::initializeDevice(aCompletedCB, aFactoryReset);
}





void DaliDevice::checkPresence(PresenceCB aPresenceResultHandler)
{
  // query the device
  brightnessDimmer->updateStatus(boost::bind(&DaliDevice::checkPresenceResponse, this, aPresenceResultHandler));
}


void DaliDevice::checkPresenceResponse(PresenceCB aPresenceResultHandler)
{
  // present if a proper YES (without collision) received
  aPresenceResultHandler(brightnessDimmer->isPresent);
}



void DaliDevice::disconnect(bool aForgetParams, DisconnectCB aDisconnectResultHandler)
{
  checkPresence(boost::bind(&DaliDevice::disconnectableHandler, this, aForgetParams, aDisconnectResultHandler, _1));
}

void DaliDevice::disconnectableHandler(bool aForgetParams, DisconnectCB aDisconnectResultHandler, bool aPresent)
{
  if (!aPresent) {
    // call inherited disconnect
    inherited::disconnect(aForgetParams, aDisconnectResultHandler);
  }
  else {
    // not disconnectable
    if (aDisconnectResultHandler) {
      aDisconnectResultHandler(false);
    }
  }
}


void DaliDevice::applyChannelValues(DoneCB aDoneCB, bool aForDimming)
{
  LightBehaviourPtr lightBehaviour = boost::dynamic_pointer_cast<LightBehaviour>(output);
  if (lightBehaviour && lightBehaviour->brightnessNeedsApplying()) {
    brightnessDimmer->setTransitionTime(lightBehaviour->transitionTimeToNewBrightness());
    // update actual dimmer value
    brightnessDimmer->setBrightness(lightBehaviour->brightnessForHardware());
    lightBehaviour->brightnessApplied(); // confirm having applied the value
  }
  inherited::applyChannelValues(aDoneCB, aForDimming);
}


// optimized DALI dimming implementation
void DaliDevice::dimChannel(DsChannelType aChannelType, DsDimMode aDimMode)
{
  // start dimming
  if (aChannelType==channeltype_brightness) {
    ChannelBehaviourPtr ch = getChannelByType(aChannelType);
    brightnessDimmer->dim(aDimMode, ch->getDimPerMS());
  }
  else {
    // not my channel, use standard implementation
    inherited::dimChannel(aChannelType, aDimMode);
  }
}



void DaliDevice::deriveDsUid()
{
  // single channel dimmer just uses dSUID derived from single DALI bus device
  dSUID = brightnessDimmer->dSUID;
}


string DaliDevice::hardwareGUID()
{
  if (brightnessDimmer->deviceInfo.gtin==0)
    return ""; // none
  // return as GS1 element strings
  return string_format("gs1:(01)%llu(21)%llu", brightnessDimmer->deviceInfo.gtin, brightnessDimmer->deviceInfo.serialNo);
}


string DaliDevice::hardwareModelGUID()
{
  if (brightnessDimmer->deviceInfo.oem_gtin==0)
    return ""; // none
  // return as GS1 element strings with Application Identifier 01=GTIN
  return string_format("gs1:(01)%llu", brightnessDimmer->deviceInfo.gtin);
}


string DaliDevice::oemGUID()
{
  if (brightnessDimmer->deviceInfo.oem_gtin==0)
    return ""; // none
  // return as GS1 element strings with Application Identifiers 01=GTIN and 21=Serial
  return string_format("gs1:(01)%llu(21)%llu", brightnessDimmer->deviceInfo.oem_gtin, brightnessDimmer->deviceInfo.oem_serialNo);
}


string DaliDevice::description()
{
  string s = inherited::description();
  s.append(brightnessDimmer->deviceInfo.description());
  return s;
}


#pragma mark - DaliRGBWDevice (multi-channel color lamp)


DaliRGBWDevice::DaliRGBWDevice(DaliDeviceContainer *aClassContainerP) :
  Device((DeviceClassContainer *)aClassContainerP)
{
  // DALI devices are always light (in this implementation, at least)
  setPrimaryGroup(group_yellow_light);
}


void DaliRGBWDevice::willBeAdded()
{
  // Note: setting up behaviours late, because we want the brightness dimmer already assigned for the hardware name
  // set up dS behaviour for color lights, which include a color scene table
  installSettings(DeviceSettingsPtr(new ColorLightDeviceSettings(*this)));
  // set the behaviour
  RGBColorLightBehaviourPtr cl = RGBColorLightBehaviourPtr(new RGBColorLightBehaviour(*this));
  cl->setHardwareOutputConfig(outputFunction_colordimmer, usage_undefined, true, 0); // DALI lights are always dimmable, no power known
  cl->setHardwareName(string_format("DALI color light"));
  cl->initMinBrightness(0.4); // min brightness is 0.4 (~= 1/256)
  addBehaviour(cl);
  // now derive dSUID
  deriveDsUid();
}


DaliDeviceContainer &DaliRGBWDevice::daliDeviceContainer()
{
  return *(static_cast<DaliDeviceContainer *>(classContainerP));
}



bool DaliRGBWDevice::getDeviceIcon(string &aIcon, bool aWithData, const char *aResolutionPrefix)
{
  if (getIcon("dali_color", aIcon, aWithData, aResolutionPrefix))
    return true;
  else
    return inherited::getDeviceIcon(aIcon, aWithData, aResolutionPrefix);
}


string DaliRGBWDevice::getExtraInfo()
{
  string s = string_format(
    "DALI short addresses: Red:%d, Green:%d, Blue:%d",
    dimmers[dimmer_red]->deviceInfo.shortAddress,
    dimmers[dimmer_green]->deviceInfo.shortAddress,
    dimmers[dimmer_blue]->deviceInfo.shortAddress
  );
  if (dimmers[dimmer_white]) {
    string_format_append(s, ", White:%d", dimmers[dimmer_white]->deviceInfo.shortAddress);
  }
  return s;
}


bool DaliRGBWDevice::addDimmer(DaliBusDevicePtr aDimmerBusDevice, string aDimmerType)
{
  if (aDimmerType=="R")
    dimmers[dimmer_red] = aDimmerBusDevice;
  else if (aDimmerType=="G")
    dimmers[dimmer_green] = aDimmerBusDevice;
  else if (aDimmerType=="B")
    dimmers[dimmer_blue] = aDimmerBusDevice;
  else if (aDimmerType=="W")
    dimmers[dimmer_white] = aDimmerBusDevice;
  else
    return false; // cannot add
  return true; // added ok
}



void DaliRGBWDevice::initializeDevice(CompletedCB aCompletedCB, bool aFactoryReset)
{
  // - sync cached channel values from actual devices
  updateNextDimmer(aCompletedCB, aFactoryReset, dimmer_red, ErrorPtr());
}


void DaliRGBWDevice::updateNextDimmer(CompletedCB aCompletedCB, bool aFactoryReset, DimmerIndex aDimmerIndex, ErrorPtr aError)
{
  if (!Error::isOK(aError)) {
    LOG(LOG_ERR, "DaliRGBWDevice: error getting state/params from dimmer#%d: %s\n", aDimmerIndex-1, aError->description().c_str());
  }
  while (aDimmerIndex<numDimmers) {
    DaliBusDevicePtr di = dimmers[aDimmerIndex];
    // process this dimmer if it exists
    if (di) {
      di->updateParams(boost::bind(&DaliRGBWDevice::updateNextDimmer, this, aCompletedCB, aFactoryReset, aDimmerIndex+1, _1));
      return; // return now, will be called again when update is complete
    }
    aDimmerIndex++; // next
  }
  // all updated (not necessarily successfully) if we land here
  RGBColorLightBehaviourPtr cl = boost::dynamic_pointer_cast<RGBColorLightBehaviour>(output);
  double r = dimmers[dimmer_red] ? dimmers[dimmer_red]->currentBrightness : 0;
  double g = dimmers[dimmer_green] ? dimmers[dimmer_green]->currentBrightness : 0;
  double b = dimmers[dimmer_blue] ? dimmers[dimmer_blue]->currentBrightness : 0;
  if (dimmers[dimmer_white]) {
    double w = dimmers[dimmer_white]->currentBrightness;
    cl->setRGBW(r, g, b, w, 255);
  }
  else {
    cl->setRGB(r, g, b, 255);
  }
  // complete - continue with initialisation in superclasses
  inherited::initializeDevice(aCompletedCB, aFactoryReset);
}



DaliBusDevicePtr DaliRGBWDevice::firstBusDevice()
{
  for (DimmerIndex idx=dimmer_red; idx<numDimmers; idx++) {
    if (dimmers[idx]) {
      return dimmers[idx];
    }
  }
  return DaliBusDevicePtr(); // none
}



void DaliRGBWDevice::checkPresence(PresenceCB aPresenceResultHandler)
{
  // assuming all channels in the same physical device, check only first one
  DaliBusDevicePtr dimmer = firstBusDevice();
  if (dimmer) {
    dimmer->updateStatus(boost::bind(&DaliRGBWDevice::checkPresenceResponse, this, aPresenceResultHandler, dimmer));
    return;
  }
  // no dimmer -> not present
  aPresenceResultHandler(false);
}


void DaliRGBWDevice::checkPresenceResponse(PresenceCB aPresenceResultHandler, DaliBusDevicePtr aDimmer)
{
  // present if a proper YES (without collision) received
  aPresenceResultHandler(aDimmer->isPresent);
}



void DaliRGBWDevice::disconnect(bool aForgetParams, DisconnectCB aDisconnectResultHandler)
{
  checkPresence(boost::bind(&DaliRGBWDevice::disconnectableHandler, this, aForgetParams, aDisconnectResultHandler, _1));
}

void DaliRGBWDevice::disconnectableHandler(bool aForgetParams, DisconnectCB aDisconnectResultHandler, bool aPresent)
{
  if (!aPresent) {
    // call inherited disconnect
    inherited::disconnect(aForgetParams, aDisconnectResultHandler);
  }
  else {
    // not disconnectable
    if (aDisconnectResultHandler) {
      aDisconnectResultHandler(false);
    }
  }
}


void DaliRGBWDevice::applyChannelValues(DoneCB aDoneCB, bool aForDimming)
{
  RGBColorLightBehaviourPtr cl = boost::dynamic_pointer_cast<RGBColorLightBehaviour>(output);
  if (cl) {
    if (needsToApplyChannels()) {
      // needs update
      // - derive (possibly new) color mode from changed channels
      cl->deriveColorMode();
      // transition time is that of the brightness channel
      MLMicroSeconds tt = cl->transitionTimeToNewBrightness();
      // RGB lamp, get components
      double r,g,b,w;
      if (dimmers[dimmer_white]) {
        // RGBW
        cl->getRGBW(r, g, b, w, 100); // dali dimmers use abstracted 0..100% brightness as input
        if (!aForDimming) LOG(LOG_INFO,
          "DALI composite RGB device %s: R=%d, G=%d, B=%d, W=%d\n",
          shortDesc().c_str(),
          (int)r, (int)g, (int)b, (int)w
        );
        dimmers[dimmer_white]->setTransitionTime(tt);
      }
      else {
        // RGB
        cl->getRGB(r, g, b, 100); // dali dimmers use abstracted 0..100% brightness as input
        if (!aForDimming) LOG(LOG_INFO,
          "DALI composite RGBW device %s: R=%d, G=%d, B=%d\n",
          shortDesc().c_str(),
          (int)r, (int)g, (int)b
        );
      }
      // set transition time for all dimmers to brightness transition time
      dimmers[dimmer_red]->setTransitionTime(tt);
      dimmers[dimmer_green]->setTransitionTime(tt);
      dimmers[dimmer_blue]->setTransitionTime(tt);
      // apply new values
      dimmers[dimmer_red]->setBrightness(r);
      dimmers[dimmer_green]->setBrightness(g);
      dimmers[dimmer_blue]->setBrightness(b);
      if (dimmers[dimmer_white]) dimmers[dimmer_white]->setBrightness(w);
    } // if needs update
    // anyway, applied now
    cl->appliedColorValues();
  }
  // confirm done
  inherited::applyChannelValues(aDoneCB, aForDimming);
}



void DaliRGBWDevice::deriveDsUid()
{
  // Multi-channel DALI devices construct their ID from UUIDs of the DALI devices involved,
  // but in a way that allows re-assignment of R/G/B without changing the dSUID
  DsUid vdcNamespace(DSUID_P44VDC_NAMESPACE_UUID);
  string mixID;
  for (DimmerIndex idx=dimmer_red; idx<numDimmers; idx++) {
    if (dimmers[idx]) {
      // use this dimmer's dSUID as part of the mix
      string dimID = dimmers[idx]->dSUID.getBinary();
      if (mixID.empty()) {
        mixID = dimID;
      }
      else {
        // xor into mix, order of dimmers does not matter for this
        for (size_t i=0; i<mixID.size(); i++) {
          mixID[i] = mixID[i] ^ dimID[i];
        }
      }
    }
  }
  // use xored ID as base for creating UUIDv5 in vdcNamespace
  dSUID.setNameInSpace("dalicombi:"+mixID, vdcNamespace);
}


string DaliRGBWDevice::hardwareGUID()
{
  DaliBusDevicePtr dimmer = firstBusDevice();
  if (!dimmer || dimmer->deviceInfo.gtin==0)
    return ""; // none
  // return as GS1 element strings
  return string_format("gs1:(01)%llu(21)%llu", dimmer->deviceInfo.gtin, dimmer->deviceInfo.serialNo);
}


string DaliRGBWDevice::hardwareModelGUID()
{
  DaliBusDevicePtr dimmer = firstBusDevice();
  if (!dimmer || dimmer->deviceInfo.oem_gtin==0)
    return ""; // none
  // return as GS1 element strings with Application Identifier 01=GTIN
  return string_format("gs1:(01)%llu", dimmer->deviceInfo.gtin);
}


string DaliRGBWDevice::oemGUID()
{
  DaliBusDevicePtr dimmer = firstBusDevice();
  if (!dimmer || dimmer->deviceInfo.oem_gtin==0)
    return ""; // none
  // return as GS1 element strings with Application Identifiers 01=GTIN and 21=Serial
  return string_format("gs1:(01)%llu(21)%llu", dimmer->deviceInfo.oem_gtin, dimmer->deviceInfo.oem_serialNo);
}


string DaliRGBWDevice::description()
{
  string s = inherited::description();
  DaliBusDevicePtr dimmer = firstBusDevice();
  if (dimmer) s.append(dimmer->deviceInfo.description());
  return s;
}


ErrorPtr DaliRGBWDevice::handleMethod(VdcApiRequestPtr aRequest, const string &aMethod, ApiValuePtr aParams)
{
  ErrorPtr respErr;
  if (aMethod=="x-p44-ungroupDevice") {
    // Remove this device from the installation, forget the settings
    respErr = daliDeviceContainer().ungroupDevice(DaliRGBWDevicePtr(this), aRequest);
  }
  else {
    respErr = inherited::handleMethod(aRequest, aMethod, aParams);
  }
  return respErr;
}



