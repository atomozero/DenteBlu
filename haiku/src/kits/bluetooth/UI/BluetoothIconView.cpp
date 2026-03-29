/*
 * BluetoothIconView stub — minimal implementation for linking.
 */
#include <BluetoothIconView.h>

namespace Bluetooth {

BBitmap* BluetoothIconView::fBitmap = NULL;
int32 BluetoothIconView::fRefCount = 0;

BluetoothIconView::BluetoothIconView()
	: BView(BRect(0, 0, 31, 31), "BluetoothIconView",
		B_FOLLOW_NONE, B_WILL_DRAW)
{
}

BluetoothIconView::~BluetoothIconView()
{
}

void BluetoothIconView::Draw(BRect rect)
{
}

} /* namespace Bluetooth */
