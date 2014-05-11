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

#include "deviceclasscontainer.hpp"

#include "device.hpp"

using namespace p44;


DeviceClassContainer::DeviceClassContainer(int aInstanceNumber, DeviceContainer *aDeviceContainerP, int aTag) :
  inherited(aDeviceContainerP),
  instanceNumber(aInstanceNumber),
  tag(aTag)
{
}


void DeviceClassContainer::addClassToDeviceContainer()
{
  // derive dSUID first, as it will be mapped by dSUID in the device container 
  deriveDsUid();
  // add to container
  getDeviceContainer().addDeviceClassContainer(DeviceClassContainerPtr(this));
}




void DeviceClassContainer::initialize(CompletedCB aCompletedCB, bool aFactoryReset)
{
  // done
	aCompletedCB(ErrorPtr()); // default to error-free initialisation
}


void DeviceClassContainer::selfTest(CompletedCB aCompletedCB)
{
  // by default, assume everything ok
  aCompletedCB(ErrorPtr());
//%%%  aCompletedCB(ErrorPtr(new Error(-42, "universal error :-)")));
}


const char *DeviceClassContainer::getPersistentDataDir()
{
	return deviceContainerP->getPersistentDataDir();
}


int DeviceClassContainer::getInstanceNumber() const
{
	return instanceNumber;
}


void DeviceClassContainer::deriveDsUid()
{
  // class containers have v5 UUIDs based on the device container's master UUID as namespace
  string name = string_format("%s.%d", deviceClassIdentifier(), getInstanceNumber()); // name is class identifier plus instance number: classID.instNo
  dSUID.setNameInSpace(name, getDeviceContainer().dSUID); // domain is dSUID of device container
}


string DeviceClassContainer::deviceClassContainerInstanceIdentifier() const
{
  string s(deviceClassIdentifier());
  string_format_append(s, ".%d@", getInstanceNumber());
  s.append(deviceContainerP->dSUID.getString());
  return s;
}


// add a device
bool DeviceClassContainer::addDevice(DevicePtr aDevice)
{
  // announce to global device container
  if (deviceContainerP->addDevice(aDevice)) {
    // not a duplicate
    // - save in my own list
    devices.push_back(aDevice);
    // added
    return true;
  }
  return false;
}



// remove a device
void DeviceClassContainer::removeDevice(DevicePtr aDevice, bool aForget)
{
	// find and remove from my list.
	for (DeviceVector::iterator pos = devices.begin(); pos!=devices.end(); ++pos) {
		if (*pos==aDevice) {
			devices.erase(pos);
			break;
		}
	}
  // announce to global device container
  deviceContainerP->removeDevice(aDevice, aForget);
}


void DeviceClassContainer::removeDevices(bool aForget)
{
	for (DeviceVector::iterator pos = devices.begin(); pos!=devices.end(); ++pos) {
    DevicePtr dev = *pos;
    deviceContainerP->removeDevice(dev, aForget);
  }
  // clear my own list
  devices.clear();
}






#pragma mark - property access

static char deviceclass_key;
static char device_container_key;
static char device_key;

enum {
  webui_url_key,
  devices_key,
  numClassContainerProperties
};



int DeviceClassContainer::numProps(int aDomain, PropertyDescriptorPtr aParentDescriptor)
{
  if (aParentDescriptor && aParentDescriptor->hasObjectKey(device_container_key)) {
    return (int)devices.size();
  }
  return inherited::numProps(aDomain, aParentDescriptor)+numClassContainerProperties;
}


PropertyDescriptorPtr DeviceClassContainer::getDescriptorByName(string aPropMatch, int &aStartIndex, int aDomain, PropertyDescriptorPtr aParentDescriptor)
{
  if (aParentDescriptor && aParentDescriptor->hasObjectKey(device_container_key)) {
    // accessing one of the devices by numeric index
    return getDescriptorByNumericName(
      aPropMatch, aStartIndex, aDomain, aParentDescriptor,
      OKEY(device_key)
    );
  }
  // None of the containers within Device - let base class handle Device-Level properties
  return inherited::getDescriptorByName(aPropMatch, aStartIndex, aDomain, aParentDescriptor);
}


PropertyContainerPtr DeviceClassContainer::getContainer(PropertyDescriptorPtr &aPropertyDescriptor, int &aDomain)
{
  if (aPropertyDescriptor->isArrayContainer()) {
    // local container
    return PropertyContainerPtr(this); // handle myself
  }
  else if (aPropertyDescriptor->hasObjectKey(device_key)) {
    // - get device
    PropertyContainerPtr container = devices[aPropertyDescriptor->fieldKey()];
    aPropertyDescriptor.reset(); // next level is "root" again (is a DsAddressable)
    return container;
  }
  // unknown here
  return NULL;
}



// note: is only called when getDescriptorByName does not resolve the name
PropertyDescriptorPtr DeviceClassContainer::getDescriptorByIndex(int aPropIndex, int aDomain, PropertyDescriptorPtr aParentDescriptor)
{
  static const PropertyDescription properties[numClassContainerProperties] = {
    { "x-p44-webui-url", apivalue_string, webui_url_key, OKEY(deviceclass_key) },
    { "x-p44-devices", apivalue_object+propflag_container, devices_key, OKEY(device_container_key) }
  };
  int n = inherited::numProps(aDomain, aParentDescriptor);
  if (aPropIndex<n)
    return inherited::getDescriptorByIndex(aPropIndex, aDomain, aParentDescriptor); // base class' property
  aPropIndex -= n; // rebase to 0 for my own first property
  return PropertyDescriptorPtr(new StaticPropertyDescriptor(&properties[aPropIndex], aParentDescriptor));
}



bool DeviceClassContainer::accessField(PropertyAccessMode aMode, ApiValuePtr aPropValue, PropertyDescriptorPtr aPropertyDescriptor)
{
  if (aPropertyDescriptor->hasObjectKey(deviceclass_key)) {
    if (aMode==access_read) {
      switch (aPropertyDescriptor->fieldKey()) {
        case webui_url_key:
          #warning "TODO: %%% construct real device webui URL here"
          aPropValue->setStringValue("http://www.plan44.ch/automation");
          return true;
      }
    }
  }
  // not my field, let base class handle it
  return inherited::accessField(aMode, aPropValue, aPropertyDescriptor);
}


#pragma mark - description/shortDesc


string DeviceClassContainer::description()
{
  string d = string_format("%s #%d: %s\n", deviceClassIdentifier(), getInstanceNumber(), shortDesc().c_str());
  string_format_append(d, "- contains %d devices:\n", devices.size());
  for (DeviceVector::iterator pos = devices.begin(); pos!=devices.end(); ++pos) {
    d.append((*pos)->description());
  }
  return d;
}



