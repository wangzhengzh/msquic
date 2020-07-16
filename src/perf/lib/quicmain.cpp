#ifdef QUIC_CLOG
#include "quicmain.cpp.clog.h"
#endif

#include "quic_driver_run.h"

#include "ThroughputServer.h"
#include "ThroughputClient.h"

const QUIC_API_TABLE* MsQuic;
uint8_t SelfSignedSecurityHash[20];
bool IsSelfSignedValid{ false };

struct CMsQuic {
    CMsQuic() :
        Result{MsQuicOpen(&MsQuic)}
    {
    }
    ~CMsQuic()
    {
        if (IsValid()) {
            MsQuicClose(MsQuic);
        }
    }
    bool IsValid() const { return QUIC_SUCCEEDED(Result); }
    QUIC_STATUS Result;
};

struct QuicMainStore {
    CMsQuic MsQuicHolder;
    UniquePtr<PerfRunner> TestToRun;
};

QuicMainStore* MainStore = nullptr;

int
QuicMainStart(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[],
    _In_ QUIC_EVENT StopEvent
    ) {
    auto TestName = GetValue(argc, argv, "TestName");
    uint8_t ServerMode = 0;
    TryGetValue(argc, argv, "ServerMode", &ServerMode);

    if (!TestName) {
        WriteOutput("Must have a TestName specified. Ex: -TestName:Throughput\n");
        return QUIC_RUN_MISSING_TEST_TYPE;
    }

    UniquePtr<QuicMainStore> LocalStore{new QuicMainStore};

    if (!LocalStore || !LocalStore->MsQuicHolder.IsValid()) {
        return QUIC_RUN_FAILED_QUIC_OPEN;
    }

    auto& TestToRun = LocalStore->TestToRun;

    if (IsValue(TestName, "Throughput")) {
        if (ServerMode) {
            TestToRun.reset(new ThroughputServer{});
        } else {
            TestToRun.reset(new ThroughputClient{});
        }
    } else {
        return QUIC_RUN_UNKNOWN_TEST_TYPE;
    }

    if (TestToRun) {
        if (QUIC_SUCCEEDED(TestToRun->Init(argc, argv))) {
            MainStore = LocalStore.release();
            return TestToRun->Start(StopEvent);
        }

    }

    return QUIC_RUN_FAILED_TEST_INITIALIZE;
}

int
QuicMainStop(
    _In_ int Timeout
    ) {
    if (!MainStore) {
        return QUIC_RUN_SUCCESS;
    }

    QUIC_STATUS Status = MainStore->TestToRun->Wait(Timeout);
    delete MainStore;
    if (QUIC_SUCCEEDED(Status)) {
        return QUIC_RUN_SUCCESS;
    }
    return QUIC_RUN_STOP_FAILURE;
}