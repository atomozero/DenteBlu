/*
 * ConnectionView stub — minimal implementation for linking.
 */
#include <ConnectionView.h>
#include <BluetoothIconView.h>

namespace Bluetooth {

ConnectionView::ConnectionView(BRect frame, BString device, BString address)
	: BView(frame, "ConnectionView", B_FOLLOW_ALL, B_WILL_DRAW),
	  fIcon(NULL),
	  fMessage(NULL),
	  fDeviceLabel(NULL),
	  fDeviceText(NULL),
	  fAddressLabel(NULL),
	  fAddressText(NULL)
{
}

void ConnectionView::Pulse()
{
}

} /* namespace Bluetooth */
