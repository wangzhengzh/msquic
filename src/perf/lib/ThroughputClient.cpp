/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC Perf Throughput Client Implementation.

--*/

#ifdef QUIC_CLOG
#include "ThroughputClient.cpp.clog.h"
#endif

//
// This needs to be included in kernel mode for QUIC_ADDR to work. Breaks user
// mode because of QUIC_TEST_API, so only included in kernel mode
//
#ifdef _KERNEL_MODE
#include <quic_platform.h>
#endif
#include "ThroughputClient.h"
#include "ThroughputCommon.h"
#include "quic_trace.h"

ThroughputClient::ThroughputClient(
    ) {
    QuicZeroMemory(&LocalIpAddr, sizeof(LocalIpAddr));
    if (Session.IsValid()) {
        Session.SetAutoCleanup();
    }
}

QUIC_STATUS
ThroughputClient::Init(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    ) {
    if (!Session.IsValid()) {
        return Session.GetInitStatus();
    }

    Port = THROUGHPUT_DEFAULT_PORT;
    TryGetValue(argc, argv, "port", &Port);

    const char* Target;
    if (!TryGetValue(argc, argv, "target", &Target)) {
        WriteOutput("Must specify '-target' argument!\n");
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    uint16_t Ip;
    if (TryGetValue(argc, argv, "ip", &Ip)) {
        switch (Ip) {
        case 4: RemoteFamily = AF_INET; break;
        case 6: RemoteFamily = AF_INET6; break;
        }
    }

    TryGetValue(argc, argv, "length", &Length);
    if (Length == 0) {
        WriteOutput("Must specify a positive 'length'\n");
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    const char* LocalAddress = nullptr;
    if (TryGetValue(argc, argv, "bind", &LocalAddress)) {
        if (!ConvertArgToAddress(LocalAddress, 0, &LocalIpAddr)) {
            WriteOutput("Failed to decode IP address: '%s'!\nMust be *, a IPv4 or a IPv6 address.\n", LocalAddress);
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    }

    // TODO: Core, since we need to support kernel mode
#ifdef QUIC_COMPARTMENT_ID
    uint16_t CompartmentId;
    if (TryGetValue(argc, argv, "comp",  &CompartmentId)) {
        NETIO_STATUS status;
        if (!NETIO_SUCCESS(status = QuicCompartmentIdSetCurrent(CompartmentId))) {
            WriteOutput("Failed to set compartment ID = %d: 0x%x\n", CompartmentId, status);
            return QUIC_STATUS_INVALID_PARAMETER;
        } else {
            WriteOutput("Running in Compartment %d\n", CompartmentId);
        }
    }
#endif

#ifdef QuicSetCurrentThreadAffinityMask
    uint8_t CpuCore;
    if (TryGetValue(argc, argv, "core",  &CpuCore)) {
        QuicSetCurrentThreadAffinityMask((DWORD_PTR)(1ull << CpuCore));
    }
#endif

    TryGetValue(argc, argv, "sendbuf", &UseSendBuffer);

    IoSize = UseSendBuffer ? THROUGHPUT_DEFAULT_IO_SIZE_BUFFERED : THROUGHPUT_DEFAULT_IO_SIZE_NONBUFFERED;
    TryGetValue(argc, argv, "iosize", &IoSize);

    IoCount = UseSendBuffer ? THROGHTPUT_DEFAULT_SEND_COUNT_BUFFERED : THROUGHPUT_DEFAULT_SEND_COUNT_NONBUFFERED;
    TryGetValue(argc, argv, "iocount", &IoCount);

    size_t Len = strlen(Target);
    TargetData.reset(new char[Len + 1]);
    QuicCopyMemory(TargetData.get(), Target, Len);
    TargetData[Len] = '\0';

    BufferAllocator.Initialize(IoSize);

    return QUIC_STATUS_SUCCESS;
}

struct ThroughputClient::SendRequest {
    QUIC_SEND_FLAGS Flags {QUIC_SEND_FLAG_NONE};
    QUIC_BUFFER QuicBuffer;
    ThroughputClient* Client;
    uint32_t IoSize;
    SendRequest(
        ThroughputClient* Client,
        uint32_t Size,
        bool FillBuffer
        ) {
        this->Client = Client;
        IoSize = Size;
        QuicBuffer.Buffer = Client->BufferAllocator.Alloc();
        if (FillBuffer) {
            memset(QuicBuffer.Buffer, 0xBF, IoSize);
        }
        QuicBuffer.Length = 0;
    }

    ~SendRequest() {
        Client->BufferAllocator.Free(QuicBuffer.Buffer);
    }

    void SetLength(
        uint64_t BytesLeftToSend
        ) {
        if (BytesLeftToSend > IoSize) {
            QuicBuffer.Length = IoSize;
        } else {
            Flags |= QUIC_SEND_FLAG_FIN;
            QuicBuffer.Length = (uint32_t)BytesLeftToSend;
        }
    }
};

struct ShutdownWrapper {
    HQUIC ConnHandle {nullptr};
    ~ShutdownWrapper() {
        if (ConnHandle) {
            MsQuic->ConnectionShutdown(ConnHandle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
    }
};

QUIC_STATUS
ThroughputClient::Start(
    _In_ QUIC_EVENT StopEvnt
    ) {
    ShutdownWrapper Shutdown;
    ConnectionData* ConnData = ConnectionDataAllocator.Alloc(this);
    if (!ConnData) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    QUIC_STATUS Status =
        MsQuic->ConnectionOpen(
            Session,
            [](HQUIC Handle, void* Context, QUIC_CONNECTION_EVENT* Event) -> QUIC_STATUS {
                ConnectionData* ConnData = (ConnectionData*)Context;
                return ConnData->Client->
                    ConnectionCallback(
                        Handle,
                        Event,
                        ConnData);
            },
            ConnData,
            &ConnData->Connection.Handle);
    if (QUIC_FAILED(Status)) {
        WriteOutput("Failed ConnectionOpen 0x%x\n", Status);
        ConnectionDataAllocator.Free(ConnData);
        return Status;
    }

    Shutdown.ConnHandle = ConnData->Connection.Handle;

    uint32_t SecFlags = QUIC_CERTIFICATE_FLAG_DISABLE_CERT_VALIDATION;
    Status =
        MsQuic->SetParam(
            ConnData->Connection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_CERT_VALIDATION_FLAGS,
            sizeof(SecFlags),
            &SecFlags);
    if (QUIC_FAILED(Status)) {
        WriteOutput("Failed Cert Validation Disable 0x%x\n", Status);
        return Status;
    }

    if (!UseSendBuffer) {
        BOOLEAN Opt = FALSE;
        Status =
            MsQuic->SetParam(
                ConnData->Connection,
                QUIC_PARAM_LEVEL_CONNECTION,
                QUIC_PARAM_CONN_SEND_BUFFERING,
                sizeof(Opt),
                &Opt);
        if (QUIC_FAILED(Status)) {
            WriteOutput("Failed Disable Send Buffering 0x%x\n", Status);
            return Status;
        }
    }

    if (QuicAddrGetFamily(&LocalIpAddr) != AF_UNSPEC) {
        MsQuic->SetParam(
            ConnData->Connection,
            QUIC_PARAM_LEVEL_CONNECTION,
            QUIC_PARAM_CONN_LOCAL_ADDRESS,
            sizeof(LocalIpAddr),
            &LocalIpAddr);
    }

    Status =
        MsQuic->ConnectionStart(
            ConnData->Connection,
            RemoteFamily,
            TargetData.get(),
            Port);
    if (QUIC_FAILED(Status)) {
        WriteOutput("Failed ConnectionStart 0x%x\n", Status);
        return Status;
    }

    StreamData* StrmData = StreamDataAllocator.Alloc(this, ConnData->Connection);

    Status =
        MsQuic->StreamOpen(
            ConnData->Connection,
            QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
            [](HQUIC Handle, void* Context, QUIC_STREAM_EVENT* Event) -> QUIC_STATUS {
                return ((StreamData*)Context)->Client->
                    StreamCallback(
                        Handle,
                        Event,
                        (StreamData*)Context);
            },
            StrmData,
            &StrmData->Stream.Handle);
    if (QUIC_FAILED(Status)) {
        WriteOutput("Failed StreamOpen 0x%x\n", Status);
        StreamDataAllocator.Free(StrmData);
        return Status;
    }

    Status =
        MsQuic->StreamStart(
            StrmData->Stream.Handle,
            QUIC_STREAM_START_FLAG_NONE);
    if (QUIC_FAILED(Status)) {
        WriteOutput("Failed StreamStart 0x%x\n", Status);
        StreamDataAllocator.Free(StrmData);
        return Status;
    }

    this->StopEvent = StopEvnt;
    StrmData->StartTime = QuicTimeUs64();

    if (Length == 0) {
        Status =
            MsQuic->StreamShutdown(
                StrmData->Stream.Handle,
                QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL,
                0);
        WriteOutput("Failed StreamShutdown with 0 length 0x%x\n", Status);
        return Status;
    }

    uint32_t SendRequestCount = 0;
    while (StrmData->BytesSent < Length && SendRequestCount < IoCount) {
        SendRequest* SendReq = SendRequestAllocator.Alloc(this, IoSize, true);
        SendReq->SetLength(Length - StrmData->BytesSent);
        StrmData->BytesSent += SendReq->QuicBuffer.Length;
        ++SendRequestCount;
        Status =
            MsQuic->StreamSend(
                StrmData->Stream,
                &SendReq->QuicBuffer,
                1,
                SendReq->Flags,
                SendReq);
        if (QUIC_FAILED(Status)) {
            WriteOutput("Failed StreamSend 0x%x\n", Status);
            SendRequestAllocator.Free(SendReq);
            return Status;
        }
    }
    WriteOutput("Started!\n");
    Shutdown.ConnHandle = nullptr;
    return Status;
}

QUIC_STATUS
ThroughputClient::Wait(
    _In_ int Timeout
    ) {
    if (Timeout > 0) {
        QuicEventWaitWithTimeout(StopEvent, Timeout);
    } else {
        QuicEventWaitForever(StopEvent);
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS
ThroughputClient::ConnectionCallback(
    _In_ HQUIC /*ConnectionHandle*/,
    _Inout_ QUIC_CONNECTION_EVENT* Event,
    _Inout_ ConnectionData* ConnData
    ) {
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        ConnectionDataAllocator.Free(ConnData);
        QuicEventSet(StopEvent);
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS
ThroughputClient::StreamCallback(
    _In_ HQUIC StreamHandle,
    _Inout_ QUIC_STREAM_EVENT* Event,
    _Inout_ StreamData* StrmData
    ) {
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        SendRequest* Req = (SendRequest*)Event->SEND_COMPLETE.ClientContext;
        if (!Event->SEND_COMPLETE.Canceled) {
            uint64_t BytesLeftToSend = Length - StrmData->BytesSent;
            StrmData->BytesCompleted += Req->QuicBuffer.Length;
            if (BytesLeftToSend != 0) {
                Req->SetLength(BytesLeftToSend);
                StrmData->BytesSent += Req->QuicBuffer.Length;

                if (QUIC_SUCCEEDED(
                    MsQuic->StreamSend(
                        StrmData->Stream.Handle,
                        &Req->QuicBuffer,
                        1,
                        Req->Flags,
                        Req))) {
                    Req = nullptr;
                }
            }
        }
        if (Req) {
            SendRequestAllocator.Free(Req);
        }
        break;
    }
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        MsQuic->StreamShutdown(
            StreamHandle,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND | QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE,
            0);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
        StrmData->EndTime = QuicTimeUs64();
        uint64_t ElapsedMicroseconds = StrmData->EndTime - StrmData->StartTime;
        uint32_t SendRate = (uint32_t)((StrmData->BytesCompleted * 1000 * 1000 * 8) / (1000 * ElapsedMicroseconds));

        WriteOutput("[%p][%llu] Closed [%s] after %u.%u ms. (TX %llu bytes @ %u kbps).\n",
            StrmData->Connection,
            GetStreamID(MsQuic, StreamHandle),
            "Complete",
            (uint32_t)(ElapsedMicroseconds / 1000),
            (uint32_t)(ElapsedMicroseconds % 1000),
            StrmData->BytesCompleted, SendRate);

        StreamDataAllocator.Free(StrmData);
        break;
    }
    }
    return QUIC_STATUS_SUCCESS;
}
