/*
 * bt_auto_connect — Daemon che scansiona e connette automaticamente
 * dispositivi Bluetooth audio (cuffie, speaker).
 *
 * Mostra un popup quando trova un dispositivo audio e chiede
 * se accoppiarlo. Se accettato, connette A2DP e suona un tono.
 *
 * Usage:
 *   bt_auto_connect              Scan loop con popup
 *   bt_auto_connect <BD_ADDR>    Connessione diretta (skip scan)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <IconUtils.h>
#include <Notification.h>
#include <OS.h>
#include <String.h>
#include <TranslationUtils.h>
#include <View.h>

#include <bluetooth/LocalDevice.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/DiscoveryListener.h>
#include <bluetooth/RemoteDevice.h>
#include <bluetooth/DeviceClass.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/A2dpSource.h>

using namespace Bluetooth;


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SCAN_INTERVAL_S		12
#define SCAN_DURATION_S		8
#define FRAMES_PER_SEND		5		/* Send 5 SBC frames per SendAudio call */

static volatile bool sQuit = false;


static void
SignalHandler(int)
{
	sQuit = true;
}


static bool
IsAudioDevice(uint32 devClass)
{
	return ((devClass >> 8) & 0x1F) == 0x04; /* Audio/Video major class */
}


static const char*
AudioDeviceType(uint32 devClass)
{
	uint8 minor = (devClass >> 2) & 0x3F;
	switch (minor) {
		case 0x01: return "Headset";
		case 0x02: return "Hands-Free";
		case 0x04: return "Headphones";
		case 0x05: return "Loudspeaker";
		case 0x06: return "Car Audio";
		default:   return "Audio Device";
	}
}


static void
GenerateTone(int16* buf, size_t samples, uint8 channels,
	uint32 sampleRate, double freq, uint64 offset, double totalSamples)
{
	double fadeLen = sampleRate * 0.05;
	for (size_t i = 0; i < samples; i++) {
		double t = (double)(offset + i) / (double)sampleRate;
		double env = 1.0;
		if (offset + i < (uint64)fadeLen)
			env = (double)(offset + i) / fadeLen;
		else if (offset + i > (uint64)(totalSamples - fadeLen))
			env = (totalSamples - offset - i) / fadeLen;
		if (env < 0) env = 0;

		int16 val = (int16)(sin(2.0 * M_PI * freq * t) * 24000.0 * env);
		for (uint8 ch = 0; ch < channels; ch++)
			buf[i * channels + ch] = val;
	}
}


static status_t
ConnectAndPlay(const bdaddr_t& address)
{
	A2dpSource source;

	printf("  Connecting A2DP...\n");
	status_t err = source.Connect(address);
	if (err != B_OK) {
		printf("  Connect failed: %s\n", strerror(err));
		return err;
	}

	err = source.StartStream();
	if (err != B_OK) {
		printf("  StartStream failed: %s\n", strerror(err));
		source.Disconnect();
		return err;
	}

	printf("  Streaming! Playing confirmation tone...\n");

	uint32 sampleRate = 44100;
	uint8 channels = 2;
	size_t spf = source.SamplesPerFrame();
	if (spf == 0) spf = 128;

	/* Send multiple frames per call for better throughput */
	size_t chunkSamples = spf * FRAMES_PER_SEND;
	int16* pcm = (int16*)malloc(chunkSamples * channels * sizeof(int16));
	if (pcm == NULL) {
		source.Disconnect();
		return B_NO_MEMORY;
	}

	/* Two-tone beep: A5 (880Hz) then E6 (1318Hz), 1s each */
	double freqs[] = { 880.0, 1318.5 };
	for (int t = 0; t < 2 && !sQuit; t++) {
		uint32 toneSamples = sampleRate * 1;
		uint64 sent = 0;
		while (sent < toneSamples && !sQuit) {
			size_t chunk = chunkSamples;
			if (sent + chunk > toneSamples)
				chunk = toneSamples - sent;
			GenerateTone(pcm, chunk, channels, sampleRate,
				freqs[t], sent, toneSamples);
			err = source.SendAudio(pcm, chunk);
			if (err != B_OK) {
				printf("  SendAudio failed: %s\n", strerror(err));
				break;
			}
			sent += chunk;
		}
	}

	free(pcm);
	printf("  Connected and paired!\n");
	printf("  Press Ctrl+C to disconnect.\n");

	while (!sQuit)
		snooze(500000);

	printf("  Disconnecting...\n");
	source.Disconnect();
	return B_OK;
}


static BBitmap*
LoadIcon(const char* type)
{
	/* Try device-specific icon first, then generic bluetooth */
	const char* paths[] = {
		"/boot/system/data/icons/AdwaitaLegacy/48x48/devices/audio-headphones.png",
		"/boot/system/data/icons/AdwaitaLegacy/48x48/devices/audio-headset.png",
		"/boot/system/data/icons/AdwaitaLegacy/48x48/legacy/audio-speakers.png",
		"/boot/system/data/icons/AdwaitaLegacy/48x48/devices/bluetooth.png",
		NULL
	};

	/* Pick best icon based on device type */
	int startIdx = 0;
	if (strcmp(type, "Headset") == 0 || strcmp(type, "Hands-Free") == 0)
		startIdx = 1;
	else if (strcmp(type, "Loudspeaker") == 0)
		startIdx = 2;

	for (int i = startIdx; paths[i] != NULL; i++) {
		BBitmap* bmp = BTranslationUtils::GetBitmapFile(paths[i]);
		if (bmp != NULL)
			return bmp;
	}
	/* Fallback: try from index 0 */
	for (int i = 0; i < startIdx; i++) {
		BBitmap* bmp = BTranslationUtils::GetBitmapFile(paths[i]);
		if (bmp != NULL)
			return bmp;
	}
	return NULL;
}


static bool
AskUserToPair(const char* name, const char* addr, const char* type)
{
	BString text;
	text.SetToFormat(
		"Bluetooth %s detected:\n\n"
		"  %s\n"
		"  %s\n\n"
		"Connect and pair this device?",
		type,
		name[0] ? name : "(unknown)",
		addr);

	BAlert* alert = new BAlert("DenteBlu",
		text.String(),
		"Ignore", "Connect",
		NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
	alert->SetShortcut(0, B_ESCAPE);

	/* Add icon to the alert */
	BBitmap* icon = LoadIcon(type);
	if (icon != NULL)
		alert->SetIcon(icon);

	int32 result = alert->Go();

	delete icon;
	return (result == 1);
}


int
main(int argc, char** argv)
{
	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	BApplication app("application/x-vnd.DenteBlu-AutoConnect");

	printf("DenteBlu Auto-Connect\n\n");

	LocalDevice* dev = LocalDevice::GetLocalDevice();
	if (dev == NULL) {
		printf("Error: bluetooth_server not running?\n");
		BAlert* alert = new BAlert("DenteBlu",
			"Bluetooth server not running.\n"
			"Start bluetooth_server first.",
			"OK", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->Go();
		_exit(1);
	}

	printf("Local: %s (%s)\n\n",
		bdaddrUtils::ToString(dev->GetBluetoothAddress()).String(),
		dev->GetFriendlyName().String());

	/* Direct address mode — skip scan */
	if (argc >= 2 && strchr(argv[1], ':') != NULL) {
		bdaddr_t addr = bdaddrUtils::FromString(argv[1]);
		if (bdaddrUtils::Compare(addr, bdaddrUtils::NullAddress())) {
			printf("Invalid address: %s\n", argv[1]);
			_exit(1);
		}

		const char* addrStr = argv[1];
		if (AskUserToPair("Bluetooth Device", addrStr, "Audio Device")) {
			for (int i = 0; i < 5 && !sQuit; i++) {
				printf("Attempt %d/5...\n", i + 1);
				if (ConnectAndPlay(addr) == B_OK)
					break;
				if (!sQuit) snooze(3000000);
			}
		}
		_exit(0);
	}

	/* Scan loop */
	DiscoveryAgent* agent = dev->GetDiscoveryAgent();
	if (agent == NULL) {
		printf("Error: No discovery agent.\n");
		_exit(1);
	}

	class ScanListener : public DiscoveryListener {
	public:
		bdaddr_t foundAddr;
		uint32 foundClass;
		BString foundName;
		bool hasAudio;

		ScanListener() : foundClass(0), hasAudio(false) {
			memset(&foundAddr, 0, sizeof(foundAddr));
		}

		void Reset() {
			hasAudio = false;
			foundClass = 0;
			foundName = "";
			memset(&foundAddr, 0, sizeof(foundAddr));
		}

		void DeviceDiscovered(RemoteDevice* d, DeviceClass cls) {
			bdaddr_t a = d->GetBluetoothAddress();
			uint32 c = cls.Record();
			BString name = d->GetFriendlyName(false);
			printf("  Found: %s %s class=0x%06x\n",
				bdaddrUtils::ToString(a).String(),
				name.String(), (unsigned)c);
			if (IsAudioDevice(c) && !hasAudio) {
				foundAddr = a;
				foundClass = c;
				foundName = name;
				hasAudio = true;
			}
		}
		void InquiryStarted(status_t) {}
		void InquiryCompleted(int) {}
	} listener;

	printf("Scanning for Bluetooth audio devices...\n");
	printf("Turn on your headphones/speaker in pairing mode.\n\n");

	while (!sQuit) {
		listener.Reset();

		agent->StartInquiry(0x9E8B33, &listener, SCAN_DURATION_S);
		for (int i = 0; i < SCAN_DURATION_S + 3 && !sQuit; i++) {
			snooze(1000000);
			if (listener.hasAudio) break;
		}

		/* Also check server log as fallback */
		if (!listener.hasAudio) {
			FILE* f = fopen("/tmp/bt_server.log", "r");
			if (f != NULL) {
				char line[256];
				while (fgets(line, sizeof(line), f) != NULL) {
					char* p = strstr(line, "InquiryResult: device ");
					if (p != NULL) {
						p += strlen("InquiryResult: device ");
						bdaddr_t a = bdaddrUtils::FromString(p);
						if (!bdaddrUtils::Compare(a,
								bdaddrUtils::NullAddress())) {
							listener.foundAddr = a;
							listener.hasAudio = true;
							listener.foundName = "";
						}
					}
				}
				fclose(f);
			}
		}

		if (listener.hasAudio) {
			BString addrStr = bdaddrUtils::ToString(listener.foundAddr);
			const char* type = AudioDeviceType(listener.foundClass);

			printf("\n  Audio device found: %s (%s)\n",
				addrStr.String(), type);

			if (AskUserToPair(
					listener.foundName.String(),
					addrStr.String(),
					type)) {
				printf("  User accepted pairing.\n");
				status_t err = ConnectAndPlay(listener.foundAddr);
				if (err == B_OK)
					break;
				printf("  Connection failed, will retry scan...\n\n");
			} else {
				printf("  User declined.\n\n");
			}
		} else {
			printf("  No audio devices found.\n");
		}

		if (!sQuit) {
			printf("  Next scan in %ds...\n\n", SCAN_INTERVAL_S);
			for (int i = 0; i < SCAN_INTERVAL_S && !sQuit; i++)
				snooze(1000000);
		}
	}

	printf("\nBye.\n");
	_exit(0);
}
