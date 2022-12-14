// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain

package netstack

import (
	"errors"
	"syscall/zx"
	"syscall/zx/fidl"

	"fidl/fuchsia/hardware/ethernet"
	"fidl/fuchsia/netstack"

	"gvisor.dev/gvisor/pkg/tcpip"
)

type netstackImpl struct {
	ns *Netstack
}

func (ni *netstackImpl) BridgeInterfaces(_ fidl.Context, nicids []uint32) (netstack.Result, error) {
	nics := make([]tcpip.NICID, len(nicids))
	for i, n := range nicids {
		nics[i] = tcpip.NICID(n)
	}
	ifs, err := ni.ns.Bridge(nics)
	if err != nil {
		return netstack.ResultWithMessage(err.Error()), nil
	}
	return netstack.ResultWithNicid(uint32(ifs.nicid)), nil
}

func (ni *netstackImpl) AddEthernetDevice(_ fidl.Context, topopath string, interfaceConfig netstack.InterfaceConfig, device ethernet.DeviceWithCtxInterface) (netstack.NetstackAddEthernetDeviceResult, error) {
	var result netstack.NetstackAddEthernetDeviceResult
	if ifs, err := ni.ns.addEth(topopath, interfaceConfig, &device); err != nil {
		var tcpipErr *TcpIpError
		if errors.As(err, &tcpipErr) {
			result.SetErr(int32(tcpipErr.ToZxStatus()))
		} else {
			result.SetErr(int32(zx.ErrInternal))
		}
	} else {
		result.SetResponse(netstack.NetstackAddEthernetDeviceResponse{Nicid: uint32(ifs.nicid)})
	}
	return result, nil
}
