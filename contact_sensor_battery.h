#ifndef _BATTERY_H_
#define _BATTERY_H_

#include <SinricProDevice.h>
#include <Capabilities/RangeController.h>

class Battery 
: public SinricProDevice
, public RangeController<Battery> {
  friend class RangeController<Battery>;
public:
  Battery(const String &deviceId) : SinricProDevice(deviceId, "Battery") {};
};

#endif
