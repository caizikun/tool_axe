// Copyright (c) 2012, Richard Osborne, All rights reserved
// This software is freely distributable under a derivative of the
// University of Illinois/NCSA Open Source License posted in
// LICENSE.txt and at <http://github.xcore.com/>

#ifndef _NetworkLink_h
#define _NetworkLink_h

#include <stdint.h>
#include <memory>
#include <string>

namespace axe {

class NetworkLink {
public:
  static const unsigned maxFrameSize = 1500 + 18;
  virtual ~NetworkLink();
  virtual void transmitFrame(const uint8_t *data, unsigned size) = 0;
  virtual bool receiveFrame(uint8_t *data, unsigned &size) = 0;
};

std::unique_ptr<NetworkLink> createNetworkLinkTap(const std::string &ifname);
  
} // End axe namespace

#endif // _NetworkLink_h
