//
//  Copyright (c) 2013-2015 plan44.ch / Lukas Zeller, Zurich, Switzerland
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

#include "binaryinputbehaviour.hpp"

using namespace p44;

BinaryInputBehaviour::BinaryInputBehaviour(Device &aDevice) :
  inherited(aDevice),
  // persistent settings
  binInputGroup(group_black_joker),
  configuredInputType(binInpType_none),
  minPushInterval(200*MilliSecond),
  changesOnlyInterval(15*Minute), // report unchanged state updates max once every 15 minutes
  // state
  lastUpdate(Never),
  lastPush(Never),
  currentState(false)
{
  // set dummy default hardware default configuration
  setHardwareInputConfig(binInpType_none, usage_undefined, true, 15*Second);
}


void BinaryInputBehaviour::setHardwareInputConfig(DsBinaryInputType aInputType, DsUsageHint aUsage, bool aReportsChanges, MLMicroSeconds aUpdateInterval)
{
  hardwareInputType = aInputType;
  inputUsage = aUsage;
  reportsChanges = aReportsChanges;
  updateInterval = aUpdateInterval;
  // set default input mode to hardware type
  configuredInputType = hardwareInputType;
}


void BinaryInputBehaviour::updateInputState(bool aNewState)
{
  LOG(LOG_NOTICE,
    "BinaryInput[%d] '%s' in device %s received new state = %d\n",
    index, hardwareName.c_str(),  device.shortDesc().c_str(), aNewState
  );
  // always update age, even if value itself may not have changed
  MLMicroSeconds now = MainLoop::now();
  lastUpdate = now;
  // input state change is considered a (regular!) user action, have it checked globally first
  if (aNewState!=currentState) {
    device.getDeviceContainer().signalDeviceUserAction(device, true);
    // Note: even if global identify handler processes this, still report state changes (otherwise upstream could get out of sync)
  }
  // in all cases, forward binary input state changes
  if (aNewState!=currentState || now>lastPush+changesOnlyInterval) {
    // changed state or no update sent for more than changesOnlyInterval
    currentState = aNewState;
    if (lastPush==Never || now>lastPush+minPushInterval) {
      // push the new value
      if (pushBehaviourState()) {
        lastPush = now;
      }
    }
  }
}


#pragma mark - persistence implementation


// SQLIte3 table name to store these parameters to
const char *BinaryInputBehaviour::tableName()
{
  return "BinaryInputSettings";
}


// data field definitions

static const size_t numFields = 4;

size_t BinaryInputBehaviour::numFieldDefs()
{
  return inherited::numFieldDefs()+numFields;
}


const FieldDefinition *BinaryInputBehaviour::getFieldDef(size_t aIndex)
{
  static const FieldDefinition dataDefs[numFields] = {
    { "dsGroup", SQLITE_INTEGER }, // Note: don't call a SQL field "group"!
    { "minPushInterval", SQLITE_INTEGER },
    { "changesOnlyInterval", SQLITE_INTEGER },
    { "configuredInputType", SQLITE_INTEGER },
  };
  if (aIndex<inherited::numFieldDefs())
    return inherited::getFieldDef(aIndex);
  aIndex -= inherited::numFieldDefs();
  if (aIndex<numFields)
    return &dataDefs[aIndex];
  return NULL;
}


/// load values from passed row
void BinaryInputBehaviour::loadFromRow(sqlite3pp::query::iterator &aRow, int &aIndex, uint64_t *aCommonFlagsP)
{
  inherited::loadFromRow(aRow, aIndex, aCommonFlagsP);
  // get the fields
  binInputGroup = (DsGroup)aRow->get<int>(aIndex++);
  minPushInterval = aRow->get<long long int>(aIndex++);
  changesOnlyInterval = aRow->get<long long int>(aIndex++);
  configuredInputType = (DsBinaryInputType)aRow->get<int>(aIndex++);
}


// bind values to passed statement
void BinaryInputBehaviour::bindToStatement(sqlite3pp::statement &aStatement, int &aIndex, const char *aParentIdentifier, uint64_t aCommonFlags)
{
  inherited::bindToStatement(aStatement, aIndex, aParentIdentifier, aCommonFlags);
  // bind the fields
  aStatement.bind(aIndex++, binInputGroup);
  aStatement.bind(aIndex++, (long long int)minPushInterval);
  aStatement.bind(aIndex++, (long long int)changesOnlyInterval);
  aStatement.bind(aIndex++, (long long int)configuredInputType);
}



#pragma mark - property access

static char binaryInput_key;

// description properties

enum {
  hardwareInputType_key,
  inputUsage_key,
  reportsChanges_key,
  updateInterval_key,
  numDescProperties
};


int BinaryInputBehaviour::numDescProps() { return numDescProperties; }
const PropertyDescriptorPtr BinaryInputBehaviour::getDescDescriptorByIndex(int aPropIndex, PropertyDescriptorPtr aParentDescriptor)
{
  static const PropertyDescription properties[numDescProperties] = {
    { "sensorFunction", apivalue_uint64, hardwareInputType_key+descriptions_key_offset, OKEY(binaryInput_key) },
    { "inputUsage", apivalue_uint64, inputUsage_key+descriptions_key_offset, OKEY(binaryInput_key) },
    { "inputType", apivalue_bool, reportsChanges_key+descriptions_key_offset, OKEY(binaryInput_key) },
    { "updateInterval", apivalue_double, updateInterval_key+descriptions_key_offset, OKEY(binaryInput_key) },
  };
  return PropertyDescriptorPtr(new StaticPropertyDescriptor(&properties[aPropIndex], aParentDescriptor));
}


// settings properties

enum {
  group_key,
  minPushInterval_key,
  changesOnlyInterval_key,
  configuredInputType_key,
  numSettingsProperties
};


int BinaryInputBehaviour::numSettingsProps() { return numSettingsProperties; }
const PropertyDescriptorPtr BinaryInputBehaviour::getSettingsDescriptorByIndex(int aPropIndex, PropertyDescriptorPtr aParentDescriptor)
{
  static const PropertyDescription properties[numSettingsProperties] = {
    { "group", apivalue_uint64, group_key+settings_key_offset, OKEY(binaryInput_key) },
    { "minPushInterval", apivalue_double, minPushInterval_key+settings_key_offset, OKEY(binaryInput_key) },
    { "changesOnlyInterval", apivalue_double, changesOnlyInterval_key+settings_key_offset, OKEY(binaryInput_key) },
    { "sensorFunction", apivalue_uint64, configuredInputType_key+settings_key_offset, OKEY(binaryInput_key) },
  };
  return PropertyDescriptorPtr(new StaticPropertyDescriptor(&properties[aPropIndex], aParentDescriptor));
}

// state properties

enum {
  value_key,
  age_key,
  numStateProperties
};


int BinaryInputBehaviour::numStateProps() { return numStateProperties; }
const PropertyDescriptorPtr BinaryInputBehaviour::getStateDescriptorByIndex(int aPropIndex, PropertyDescriptorPtr aParentDescriptor)
{
  static const PropertyDescription properties[numStateProperties] = {
    { "value", apivalue_bool, value_key+states_key_offset, OKEY(binaryInput_key) },
    { "age", apivalue_double, age_key+states_key_offset, OKEY(binaryInput_key) },
  };
  return PropertyDescriptorPtr(new StaticPropertyDescriptor(&properties[aPropIndex], aParentDescriptor));
}


// access to all fields
bool BinaryInputBehaviour::accessField(PropertyAccessMode aMode, ApiValuePtr aPropValue, PropertyDescriptorPtr aPropertyDescriptor)
{
  if (aPropertyDescriptor->hasObjectKey(binaryInput_key)) {
    if (aMode==access_read) {
      // read properties
      switch (aPropertyDescriptor->fieldKey()) {
        // Description properties
        case hardwareInputType_key+descriptions_key_offset: // aka "hardwareSensorFunction"
          aPropValue->setUint8Value(hardwareInputType);
          return true;
        case inputUsage_key+descriptions_key_offset:
          aPropValue->setUint8Value(inputUsage);
          return true;
        case reportsChanges_key+descriptions_key_offset: // aka "inputType"
          aPropValue->setUint8Value(reportsChanges ? 1 : 0);
          return true;
        case updateInterval_key+descriptions_key_offset:
          aPropValue->setDoubleValue((double)updateInterval/Second);
          return true;
        // Settings properties
        case group_key+settings_key_offset:
          aPropValue->setUint16Value(binInputGroup);
          return true;
        case minPushInterval_key+settings_key_offset:
          aPropValue->setDoubleValue((double)minPushInterval/Second);
          return true;
        case changesOnlyInterval_key+settings_key_offset:
          aPropValue->setDoubleValue((double)changesOnlyInterval/Second);
          return true;
        case configuredInputType_key+settings_key_offset: // aka "sensorFunction"
          aPropValue->setUint8Value(configuredInputType);
          return true;
        // States properties
        case value_key+states_key_offset:
          // value
          if (lastUpdate==Never)
            aPropValue->setNull();
          else
            aPropValue->setBoolValue(currentState);
          return true;
        case age_key+states_key_offset:
          // age
          if (lastUpdate==Never)
            aPropValue->setNull();
          else
            aPropValue->setDoubleValue((double)(MainLoop::now()-lastUpdate)/Second);
          return true;
      }
    }
    else {
      // write properties
      switch (aPropertyDescriptor->fieldKey()) {
        // Settings properties
        case group_key+settings_key_offset:
          setPVar(binInputGroup, (DsGroup)aPropValue->int32Value());
          return true;
        case minPushInterval_key+settings_key_offset:
          setPVar(minPushInterval, (MLMicroSeconds)(aPropValue->doubleValue()*Second));
          return true;
        case changesOnlyInterval_key+settings_key_offset:
          setPVar(changesOnlyInterval, (MLMicroSeconds)(aPropValue->doubleValue()*Second));
          return true;
        case configuredInputType_key+settings_key_offset: // aka "sensorFunction"
          setPVar(configuredInputType, (DsBinaryInputType)aPropValue->int32Value());
          return true;
      }
    }
  }
  // not my field, let base class handle it
  return inherited::accessField(aMode, aPropValue, aPropertyDescriptor);
}



#pragma mark - description/shortDesc


string BinaryInputBehaviour::description()
{
  string s = string_format("%s behaviour\n", shortDesc().c_str());
  string_format_append(s, "- binary input type: %d, reportsChanges=%d, interval: %d mS\n", hardwareInputType, reportsChanges, updateInterval/MilliSecond);
  string_format_append(s, "- minimal interval between pushes: %d mS\n", minPushInterval/MilliSecond);
  s.append(inherited::description());
  return s;
}
