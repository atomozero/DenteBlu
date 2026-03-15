/*
 * Copyright 2007-2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 */


#ifndef _HCITRANSPORT_ACCESSOR_H_
#define _HCITRANSPORT_ACCESSOR_H_

#include "HCIDelegate.h"


class HCITransportAccessor : public HCIDelegate {

	public:
		HCITransportAccessor(BPath* path);
		~HCITransportAccessor();
		status_t IssueCommand(raw_command rc, size_t size);
		status_t Launch();
		status_t GattIoctl(uint32 op, void* data, size_t size);
	private:
		int fDescriptor;
};

#endif
