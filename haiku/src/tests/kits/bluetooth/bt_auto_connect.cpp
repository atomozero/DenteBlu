/*
 * bt_auto_connect — Daemon che scansiona e connette automaticamente
 * dispositivi Bluetooth audio (cuffie, speaker).
 *
 * Ciclo: inquiry classica → trova audio device → connette A2DP → tono
 *
 * Usage:
 *   bt_auto_connect              Scan + auto-connect loop
 *   bt_auto_connect <BD_ADDR>    Skip scan, connect directly
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <Application.h>
#include <OS.h>

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

#define SCAN_INTERVAL_US	10000000	/* 10s between scans */
#define SCAN_DURATION_S		8			/* 8s inquiry duration */
#define TONE_DURATION_S		3			/* 3s confirmation tone */

static volatile bool sQuit = false;


static void
SignalHandler(int)
{
	sQuit = true;
}


static void
GenerateTone(int16* buf, size_t samples, uint8 channels,
	uint32 sampleRate, double freq, uint64 offset, double totalSamples)
{
	double fadeLen = sampleRate * 0.05;
	for (size_t i = 0; i < samples; i++) {
		double t = (double)(offset + i) / (double)sampleRate;
		double envelope = 1.0;
		if (offset + i < (uint64)fadeLen)
			envelope = (double)(offset + i) / fadeLen;
		else if (offset + i > (uint64)(totalSamples - fadeLen))
			envelope = (totalSamples - offset - i) / fadeLen;
		if (envelope < 0) envelope = 0;

		int16 val = (int16)(sin(2.0 * M_PI * freq * t) * 24000.0 * envelope);
		for (uint8 ch = 0; ch < channels; ch++)
			buf[i * channels + ch] = val;
	}
}


static status_t
ConnectA2DP(const bdaddr_t& address)
{
	A2dpSource source;

	printf("  → Connecting A2DP to %s...\n",
		bdaddrUtils::ToString(address).String());

	status_t err = source.Connect(address);
	if (err != B_OK) {
		printf("  ✗ Connect failed: %s\n", strerror(err));
		return err;
	}

	printf("  ✓ Connected! Starting stream...\n");

	err = source.StartStream();
	if (err != B_OK) {
		printf("  ✗ StartStream failed: %s\n", strerror(err));
		source.Disconnect();
		return err;
	}

	printf("  ✓ Streaming! Playing confirmation tone...\n");

	uint32 sampleRate = 44100;
	uint8 channels = 2;
	uint32 totalSamples = sampleRate * TONE_DURATION_S;
	size_t spf = source.SamplesPerFrame();
	if (spf == 0) spf = 128;

	int16* pcm = (int16*)malloc(spf * channels * sizeof(int16));
	if (pcm == NULL) {
		source.Disconnect();
		return B_NO_MEMORY;
	}

	/* Play two-tone confirmation beep: A5 then E6 */
	double freqs[] = { 880.0, 1318.5 };
	for (int t = 0; t < 2 && !sQuit; t++) {
		uint32 toneSamples = sampleRate * 1; /* 1 second each */
		uint64 sent = 0;
		while (sent < toneSamples && !sQuit) {
			size_t chunk = spf;
			if (sent + chunk > toneSamples)
				chunk = toneSamples - sent;
			GenerateTone(pcm, chunk, channels, sampleRate,
				freqs[t], sent, toneSamples);
			err = source.SendAudio(pcm, chunk);
			if (err != B_OK) break;
			sent += chunk;
		}
	}

	free(pcm);

	printf("  ✓ Device paired and connected.\n");
	printf("    Press Ctrl+C to disconnect.\n");

	while (!sQuit)
		snooze(500000);

	printf("  Disconnecting...\n");
	source.Disconnect();
	return B_OK;
}


static bool
TryScan(LocalDevice* dev)
{
	/* Use inquiry to scan, but since the DiscoveryListener
	 * callbacks don't work reliably, we also check the server
	 * log for discovered addresses */

	DiscoveryAgent* agent = dev->GetDiscoveryAgent();
	if (agent == NULL) return false;

	class Listener : public DiscoveryListener {
	public:
		bdaddr_t found;
		bool hasAudio;
		Listener() : hasAudio(false) { memset(&found, 0, sizeof(found)); }
		void DeviceDiscovered(RemoteDevice* d, DeviceClass cls) {
			bdaddr_t a = d->GetBluetoothAddress();
			uint8 major = (cls.Record() >> 8) & 0x1F;
			printf("  Found: %s class=0x%06x %s\n",
				bdaddrUtils::ToString(a).String(),
				(unsigned)cls.Record(),
				major == 0x04 ? "← AUDIO" : "");
			if (major == 0x04 && !hasAudio) {
				found = a;
				hasAudio = true;
			}
		}
		void InquiryStarted(status_t) {}
		void InquiryCompleted(int) {}
	} listener;

	printf("  Inquiry (%ds)...\n", SCAN_DURATION_S);
	status_t err = agent->StartInquiry(0x9E8B33, &listener, SCAN_DURATION_S);
	if (err != B_OK) {
		printf("  Inquiry failed: %s\n", strerror(err));
		return false;
	}

	/* Wait for inquiry to finish */
	for (int i = 0; i < SCAN_DURATION_S + 3; i++) {
		if (sQuit) return false;
		snooze(1000000);
		if (listener.hasAudio) break;
	}

	if (listener.hasAudio) {
		printf("\n  ★ Audio device found via callback!\n");
		ConnectA2DP(listener.found);
		return true;
	}

	/* Fallback: check server log for InquiryResult addresses */
	FILE* f = fopen("/tmp/bt_server.log", "r");
	if (f == NULL) return false;

	char line[256];
	bdaddr_t lastAddr;
	bool foundInLog = false;
	memset(&lastAddr, 0, sizeof(lastAddr));

	while (fgets(line, sizeof(line), f) != NULL) {
		/* Look for: InquiryResult: device xx:xx:xx:xx:xx:xx */
		char* p = strstr(line, "InquiryResult: device ");
		if (p != NULL) {
			p += strlen("InquiryResult: device ");
			lastAddr = bdaddrUtils::FromString(p);
			if (!bdaddrUtils::Compare(lastAddr, bdaddrUtils::NullAddress()))
				foundInLog = true;
		}
	}
	fclose(f);

	if (foundInLog) {
		printf("\n  ★ Device found in server log: %s\n",
			bdaddrUtils::ToString(lastAddr).String());
		ConnectA2DP(lastAddr);
		return true;
	}

	return false;
}


int
main(int argc, char** argv)
{
	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	BApplication app("application/x-vnd.DenteBlu-AutoConnect");

	printf("╔═══════════════════════════════════════════╗\n");
	printf("║  DenteBlu Auto-Connect                    ║\n");
	printf("║  Put your BT headphones in pairing mode   ║\n");
	printf("╚═══════════════════════════════════════════╝\n\n");

	LocalDevice* dev = LocalDevice::GetLocalDevice();
	if (dev == NULL) {
		printf("Error: bluetooth_server not running?\n");
		_exit(1);
	}

	printf("Local: %s (%s)\n\n",
		bdaddrUtils::ToString(dev->GetBluetoothAddress()).String(),
		dev->GetFriendlyName().String());

	/* Direct address mode */
	if (argc >= 2 && strchr(argv[1], ':') != NULL) {
		bdaddr_t addr = bdaddrUtils::FromString(argv[1]);
		if (bdaddrUtils::Compare(addr, bdaddrUtils::NullAddress())) {
			printf("Invalid address: %s\n", argv[1]);
			_exit(1);
		}
		printf("Direct connect to %s\n", argv[1]);
		/* Try multiple times */
		for (int i = 0; i < 5 && !sQuit; i++) {
			printf("\nAttempt %d/5...\n", i + 1);
			if (ConnectA2DP(addr) == B_OK)
				break;
			if (!sQuit) snooze(3000000);
		}
		_exit(0);
	}

	/* Scan loop */
	/* Clear old inquiry results from server log */
	FILE* f = fopen("/tmp/bt_server.log.scan_marker", "w");
	if (f) { fprintf(f, "marker"); fclose(f); }

	while (!sQuit) {
		printf("────────────────────────────────\n");
		if (TryScan(dev))
			break;
		printf("  No audio device found. Retrying in %ds...\n\n",
			(int)(SCAN_INTERVAL_US / 1000000));
		for (int i = 0; i < (int)(SCAN_INTERVAL_US / 1000000) && !sQuit; i++)
			snooze(1000000);
	}

	printf("\nBye.\n");
	_exit(0);
}
