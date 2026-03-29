/*
 * PincodeWindow stub — minimal implementation for linking.
 */
#include <PincodeWindow.h>
#include <bluetooth/RemoteDevice.h>

namespace Bluetooth {

PincodeWindow::PincodeWindow(bdaddr_t address, hci_id hid)
	: BWindow(BRect(100, 100, 400, 250), "Bluetooth PIN",
		B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE),
	  fHid(hid)
{
	memcpy(&fBdaddr, &address, sizeof(bdaddr_t));
	InitUI();
}

PincodeWindow::PincodeWindow(RemoteDevice* rDevice)
	: BWindow(BRect(100, 100, 400, 250), "Bluetooth PIN",
		B_TITLED_WINDOW, B_NOT_RESIZABLE | B_NOT_ZOOMABLE),
	  fHid(-1)
{
	memset(&fBdaddr, 0, sizeof(bdaddr_t));
	InitUI();
}

void PincodeWindow::MessageReceived(BMessage* msg)
{
	BWindow::MessageReceived(msg);
}

bool PincodeWindow::QuitRequested()
{
	return true;
}

void PincodeWindow::SetBDaddr(BString address)
{
}

void PincodeWindow::InitUI()
{
	fMessage = NULL;
	fRemoteInfo = NULL;
	fAcceptButton = NULL;
	fCancelButton = NULL;
	fPincodeText = NULL;
	fIcon = NULL;
	fMessage2 = NULL;
	fDeviceLabel = NULL;
	fDeviceText = NULL;
	fAddressLabel = NULL;
	fAddressText = NULL;
}

} /* namespace Bluetooth */
