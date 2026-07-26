#pragma once

#include "net_control.hpp"

/** Best-effort home location from the public IP address.
 *
 *  Only ever a *suggestion* the user confirms in the settings menu: free IP
 *  geolocation is city-level at best, and CGNAT, mobile broadband and VPNs can
 *  put it hundreds of km out. The radar draws 25 nm across 480 px, so an
 *  unreviewed error here would visibly displace every aircraft.
 *
 *  Network task only (it does HTTP). */
bool fetchIpLocation(GeoResult *out);
