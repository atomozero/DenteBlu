/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BluetoothAudioNode — BBufferConsumer che riceve PCM dal mixer
 * di sistema e lo invia alle cuffie BT via A2dpSource.
 */
#ifndef _BLUETOOTH_AUDIO_NODE_H_
#define _BLUETOOTH_AUDIO_NODE_H_


#include <BufferConsumer.h>
#include <Controllable.h>
#include <MediaEventLooper.h>

#include <bluetooth/bluetooth.h>


namespace Bluetooth {
	class A2dpSource;
}


class BluetoothAudioAddOn;


class BluetoothAudioNode : public BMediaEventLooper,
	public BBufferConsumer, public BControllable {
public:
								BluetoothAudioNode(
									BluetoothAudioAddOn* addOn);
	virtual						~BluetoothAudioNode();

	/* BMediaNode */
	virtual	BMediaAddOn*		AddOn(int32* _internalId) const;
	virtual	void				NodeRegistered();
	virtual	status_t			HandleMessage(int32 message,
									const void* data, size_t size);

	/* BMediaEventLooper */
	virtual	void				HandleEvent(
									const media_timed_event* event,
									bigtime_t lateness,
									bool realTimeEvent = false);

	/* BBufferConsumer */
	virtual	status_t			AcceptFormat(
									const media_destination& destination,
									media_format* format);
	virtual	status_t			GetNextInput(int32* cookie,
									media_input* _input);
	virtual	void				DisposeInputCookie(int32 cookie);
	virtual	void				BufferReceived(BBuffer* buffer);
	virtual	void				ProducerDataStatus(
									const media_destination& forWhom,
									int32 status,
									bigtime_t atPerformanceTime);
	virtual	status_t			GetLatencyFor(
									const media_destination& forWhom,
									bigtime_t* _latency,
									media_node_id* _timesource);
	virtual	status_t			Connected(const media_source& producer,
									const media_destination& where,
									const media_format& withFormat,
									media_input* _input);
	virtual	void				Disconnected(
									const media_source& producer,
									const media_destination& where);
	virtual	status_t			FormatChanged(
									const media_source& producer,
									const media_destination& consumer,
									int32 changeTag,
									const media_format& format);

	/* BControllable */
	virtual	status_t			GetParameterValue(int32 id,
									bigtime_t* lastChange,
									void* value, size_t* ioSize);
	virtual	void				SetParameterValue(int32 id,
									bigtime_t when, const void* value,
									size_t size);

	/* Accessors for testing */
			float				Volume() const { return fVolume; }

	/* Audio conversion helper (public for testability) */
	static	void				ConvertFloatToInt16(const float* src,
									int16* dst, size_t sampleCount,
									float volume);

private:
			void				_InitParameterWeb();
			void				_HandleBuffer(BBuffer* buffer);
			status_t			_ReadDeviceAddress(bdaddr_t* address);

	enum {
		kParamVolume = 1
	};

			BluetoothAudioAddOn* fAddOn;
			media_input			fInput;
			bool				fInputConnected;
			float				fVolume;
			bigtime_t			fVolumeLastChange;
			Bluetooth::A2dpSource* fA2dp;
};


#endif /* _BLUETOOTH_AUDIO_NODE_H_ */
