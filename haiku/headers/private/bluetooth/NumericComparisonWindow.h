/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef _NUMERIC_COMPARISON_WINDOW_H_
#define _NUMERIC_COMPARISON_WINDOW_H_


#include <View.h>
#include <Window.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI.h>

#include <BluetoothIconView.h>

class BStringView;
class BButton;

namespace Bluetooth {


class NumericComparisonWindow : public BWindow {
public:
	/* Classic SSP constructor */
						NumericComparisonWindow(bdaddr_t address,
							hci_id hid, uint32 numericValue);
	/* BLE constructor */
						NumericComparisonWindow(bdaddr_t address,
							hci_id hid, uint32 numericValue,
							uint16 leConnHandle);

	virtual void		MessageReceived(BMessage* msg);
	virtual bool		QuitRequested();

private:
			void		_InitUI();

			bdaddr_t	fBdaddr;
			hci_id		fHid;
			uint32		fNumericValue;
			bool		fIsLE;
			uint16		fLeConnHandle;

			BluetoothIconView*	fIcon;
			BStringView*		fMessage;
			BStringView*		fMessage2;
			BStringView*		fAddressLabel;
			BStringView*		fAddressText;
			BStringView*		fNumericLabel;
			BStringView*		fNumericText;
			BButton*			fAcceptButton;
			BButton*			fCancelButton;
};


}

#ifndef _BT_USE_EXPLICIT_NAMESPACE
using Bluetooth::NumericComparisonWindow;
#endif

#endif /* _NUMERIC_COMPARISON_WINDOW_H_ */
