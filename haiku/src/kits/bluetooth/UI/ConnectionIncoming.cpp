/*
 * ConnectionIncoming stub — minimal implementation for linking.
 */
#include <ConnectionIncoming.h>

namespace Bluetooth {

ConnectionIncoming::ConnectionIncoming(bdaddr_t address)
	: BWindow(BRect(100, 100, 400, 250), "Incoming Connection",
		B_TITLED_WINDOW, B_NOT_RESIZABLE),
	  fView(NULL)
{
}

ConnectionIncoming::ConnectionIncoming(RemoteDevice* rDevice)
	: BWindow(BRect(100, 100, 400, 250), "Incoming Connection",
		B_TITLED_WINDOW, B_NOT_RESIZABLE),
	  fView(NULL)
{
}

ConnectionIncoming::~ConnectionIncoming()
{
}

void ConnectionIncoming::MessageReceived(BMessage* message)
{
	BWindow::MessageReceived(message);
}

bool ConnectionIncoming::QuitRequested()
{
	return true;
}

} /* namespace Bluetooth */
