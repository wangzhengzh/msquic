/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    Packet processing helpers (validation, encoding and tracing).

--*/

#include "precomp.h"

#ifdef QUIC_LOGS_WPP
#include "packet.tmh"
#endif

//
// The list of supported QUIC version numbers, in network byte order.
// The list is in priority order (highest to lowest).
//
const uint32_t QuicSupportedVersionList[] = {
    QUIC_VERSION_DRAFT_24,
    QUIC_VERSION_MS_1
};

const uint8_t QuicInitialSaltVersion1[] =
{
    0xc3, 0xee, 0xf7, 0x12, 0xc7, 0x2e, 0xbb, 0x5a, 0x11, 0xa7,
    0xd2, 0x43, 0x2b, 0xb4, 0x63, 0x65, 0xbe, 0xf9, 0xf5, 0x02
};

const char PacketLogPrefix[2][2] = {
    {'C', 'S'}, {'T', 'R'}
};

//
// The Long Header types that are allowed to be processed
// by a Client or Server.
//
const BOOLEAN QUIC_HEADER_TYPE_ALLOWED[2][4] = {
    //
    // Client
    //
    {
        TRUE,  // QUIC_INITIAL
        FALSE, // QUIC_0_RTT_PROTECTED
        TRUE,  // QUIC_HANDSHAKE
        TRUE,  // QUIC_RETRY
    },

    //
    // Server
    //
    {
        TRUE,  // QUIC_INITIAL
        TRUE,  // QUIC_0_RTT_PROTECTED
        TRUE,  // QUIC_HANDSHAKE
        FALSE, // QUIC_RETRY
    },
};

const uint16_t QuicMinPacketLengths[2] = { MIN_INV_SHORT_HDR_LENGTH, MIN_INV_LONG_HDR_LENGTH };

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != FALSE)
BOOLEAN
QuicPacketValidateInvariant(
    _In_ const void* Owner, // Binding or Connection depending on state
    _Inout_ QUIC_RECV_PACKET* Packet,
    _In_ BOOLEAN IsBindingShared
    )
{
    uint8_t DestCidLen, SourceCidLen;
    const uint8_t* DestCid, *SourceCid;

    //
    // Ignore empty or too short packets.
    //
    if (Packet->BufferLength == 0 ||
        Packet->BufferLength < QuicMinPacketLengths[Packet->Invariant->IsLongHeader]) {
        QuicPacketLogDrop(Owner, Packet, "Too small for Packet->Invariant");
        return FALSE;
    }

    if (Packet->Invariant->IsLongHeader) {

        Packet->IsShortHeader = FALSE;

        DestCidLen = Packet->Invariant->LONG_HDR.DestCidLength;
        if (Packet->BufferLength < MIN_INV_LONG_HDR_LENGTH + DestCidLen) {
            QuicPacketLogDrop(Owner, Packet, "LH no room for DestCid");
            return FALSE;
        }
        if (IsBindingShared && DestCidLen == 0) {
            QuicPacketLogDrop(Owner, Packet, "Zero length DestCid");
            return FALSE;
        }
        DestCid = Packet->Invariant->LONG_HDR.DestCid;

        SourceCidLen = *(DestCid + DestCidLen);
        Packet->HeaderLength = MIN_INV_LONG_HDR_LENGTH + DestCidLen + SourceCidLen;
        if (Packet->BufferLength < Packet->HeaderLength) {
            QuicPacketLogDrop(Owner, Packet, "LH no room for SourceCid");
            return FALSE;
        }
        SourceCid = DestCid + sizeof(uint8_t) + DestCidLen;

    } else {

        Packet->IsShortHeader = TRUE;
        DestCidLen = IsBindingShared ? MSQUIC_CONNECTION_ID_LENGTH : 0;
        SourceCidLen = 0;

        //
        // Header length so far (just Packet->Invariant part).
        //
        Packet->HeaderLength = sizeof(uint8_t) + DestCidLen;

        if (Packet->BufferLength < Packet->HeaderLength) {
            QuicPacketLogDrop(Owner, Packet, "SH no room for DestCid");
            return FALSE;
        }

        DestCid = Packet->Invariant->SHORT_HDR.DestCid;
        SourceCid = NULL;
    }

    if (Packet->DestCid != NULL) {

        //
        // The CID(s) have already been previously set for this UDP datagram.
        // Make sure they match.
        //

        if (Packet->DestCidLen != DestCidLen ||
            memcmp(Packet->DestCid, DestCid, DestCidLen) != 0) {
            QuicPacketLogDrop(Owner, Packet, "DestCid don't match");
            return FALSE;
        }

        if (!Packet->IsShortHeader) {

            QUIC_DBG_ASSERT(Packet->SourceCid != NULL);

            if (Packet->SourceCidLen != SourceCidLen ||
                memcmp(Packet->SourceCid, SourceCid, SourceCidLen) != 0) {
                QuicPacketLogDrop(Owner, Packet, "SourceCid don't match");
                return FALSE;
            }
        }

    } else {

        //
        // The first QUIC packet in the datagram, save the CIDs with the receive
        // context.
        //

        Packet->DestCidLen = DestCidLen;
        Packet->SourceCidLen = SourceCidLen;

        Packet->DestCid = DestCid;
        Packet->SourceCid = SourceCid;
    }

    Packet->ValidatedHeaderInv = TRUE;

    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != FALSE)
BOOLEAN
QuicPacketValidateLongHeaderV1(
    _In_ const void* Owner, // Binding or Connection depending on state
    _In_ BOOLEAN IsServer,
    _Inout_ QUIC_RECV_PACKET* Packet,
    _Outptr_result_buffer_maybenull_(*TokenLength)
        const uint8_t** Token,
    _Out_ uint16_t* TokenLength
    )
{
    //
    // The Packet->Invariant part of the header has already been validated. No need
    // to check that portion of the header again.
    //
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);
    QUIC_DBG_ASSERT(Packet->BufferLength >= Packet->HeaderLength);
    QUIC_DBG_ASSERT(Packet->LH->Type != QUIC_RETRY); // Retry uses a different code path.

    if (Packet->DestCidLen > QUIC_MAX_CONNECTION_ID_LENGTH_V1 ||
        Packet->SourceCidLen > QUIC_MAX_CONNECTION_ID_LENGTH_V1) {
        QuicPacketLogDrop(Owner, Packet, "Greater than allowed max CID length");
        return FALSE;
    }

    //
    // Validate acceptable types.
    //
    QUIC_DBG_ASSERT(IsServer == 0 || IsServer == 1);
    if (QUIC_HEADER_TYPE_ALLOWED[IsServer][Packet->LH->Type] == FALSE) {
        QuicPacketLogDropWithValue(Owner, Packet, "Invalid client/server packet type", Packet->LH->Type);
        return FALSE;
    }

    //
    // Check the Fixed bit to ensure it is set to 1.
    //
    if (Packet->LH->FixedBit == 0) {
        QuicPacketLogDrop(Owner, Packet, "Invalid LH FixedBit bits values");
        return FALSE;
    }

    //
    // Cannot validate the PnLength and Reserved fields yet, as they are
    // protected by header protection.
    //

    uint16_t Offset = Packet->HeaderLength;

    if (Packet->LH->Type == QUIC_INITIAL) {
        if (IsServer && Packet->BufferLength < QUIC_MIN_INITIAL_PACKET_LENGTH) {
            //
            // All client initial packets need to be padded to a minimum length.
            //
            QuicPacketLogDropWithValue(Owner, Packet, "Client Long header Initial packet too short", Packet->BufferLength);
            return FALSE;
        }

        QUIC_VAR_INT TokenLengthVarInt;
        if (!QuicVarIntDecode(
                Packet->BufferLength,
                Packet->Buffer,
                &Offset,
                &TokenLengthVarInt)) {
            QuicPacketLogDrop(Owner, Packet, "Long header has invalid token length");
            return FALSE;
        }

        if ((uint64_t)Packet->BufferLength < Offset + TokenLengthVarInt) {
            QuicPacketLogDropWithValue(Owner, Packet, "Long header has token length larger than buffer length", TokenLengthVarInt);
            return FALSE;
        }

        *Token = Packet->Buffer + Offset;
        *TokenLength = (uint16_t)TokenLengthVarInt;

        Offset += (uint16_t)TokenLengthVarInt;

    } else {

        *Token = NULL;
        *TokenLength = 0;
    }

    QUIC_VAR_INT LengthVarInt;
    if (!QuicVarIntDecode(
            Packet->BufferLength,
            Packet->Buffer,
            &Offset,
            &LengthVarInt)) {
        QuicPacketLogDrop(Owner, Packet, "Long header has invalid payload length");
        return FALSE;
    }

    if ((uint64_t)Packet->BufferLength < Offset + LengthVarInt) {
        QuicPacketLogDropWithValue(Owner, Packet, "Long header has length larger than buffer length", LengthVarInt);
        return FALSE;
    }

    if (Packet->BufferLength < Offset + sizeof(uint32_t)) {
        QuicPacketLogDropWithValue(Owner, Packet, "Long Header doesn't have enough room for packet number",
            Packet->BufferLength);
        return FALSE;
    }

    //
    // Packet number is still encrypted at this point, so we can't decode it
    // and therefore cannot calculate the total length of the header yet.
    // For the time being, set header length to the start of the packet
    // number and payload length to everything after that.
    //
    Packet->HeaderLength = Offset;
    Packet->PayloadLength = (uint16_t)LengthVarInt;
    Packet->BufferLength = Packet->HeaderLength + Packet->PayloadLength;
    Packet->ValidatedHeaderVer = TRUE;

    return TRUE;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicPacketDecodeRetryTokenV1(
    _In_ const QUIC_RECV_PACKET* const Packet,
    _Outptr_result_buffer_maybenull_(*TokenLength)
        const uint8_t** Token,
    _Out_ uint16_t* TokenLength
    )
{
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderVer);
    QUIC_DBG_ASSERT(Packet->Invariant->IsLongHeader);
    QUIC_DBG_ASSERT(Packet->LH->Type == QUIC_INITIAL);

    uint16_t Offset =
        sizeof(QUIC_LONG_HEADER_V1) +
        Packet->DestCidLen +
        sizeof(uint8_t) +
        Packet->SourceCidLen;

    QUIC_VAR_INT TokenLengthVarInt;
    BOOLEAN Success = QuicVarIntDecode(
        Packet->BufferLength, Packet->Buffer, &Offset, &TokenLengthVarInt);
    QUIC_DBG_ASSERT(Success); // Was previously validated.
    UNREFERENCED_PARAMETER(Success);

    QUIC_DBG_ASSERT(Offset + TokenLengthVarInt <= Packet->BufferLength); // Was previously validated.
    *Token = Packet->Buffer + Offset;
    *TokenLength = (uint16_t)TokenLengthVarInt;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
_Success_(return != FALSE)
BOOLEAN
QuicPacketValidateShortHeaderV1(
    _In_ const void* Owner, // Binding or Connection depending on state
    _Inout_ QUIC_RECV_PACKET* Packet
    )
{
    //
    // The Packet->Invariant part of the header has already been validated. No need
    // to check any additional lengths as the cleartext part of the version
    // specific header isn't any larger than the Packet->Invariant.
    //
    QUIC_DBG_ASSERT(Packet->ValidatedHeaderInv);
    QUIC_DBG_ASSERT(Packet->BufferLength >= Packet->HeaderLength);

    //
    // Check the Fixed bit to ensure it is set to 1.
    //
    if (Packet->SH->FixedBit == 0) {
        QuicPacketLogDrop(Owner, Packet, "Invalid SH FixedBit bits values");
        return FALSE;
    }

    //
    // Cannot validate the PnLength, KeyPhase and Reserved fields yet, as they
    // are protected by header protection.
    //

    //
    // Packet number is still encrypted at this point, so we can't decode it
    // and therefore cannot calculate the total length of the header yet.
    // For the time being, set header length to the start of the packet
    // number and payload length to everything after that.
    //
    Packet->PayloadLength = Packet->BufferLength - Packet->HeaderLength;
    Packet->ValidatedHeaderVer = TRUE;

    return TRUE;
}

_Null_terminated_ const char*
QuicLongHeaderTypeToString(uint8_t Type)
{
    switch (Type)
    {
    case QUIC_INITIAL:              return "I";
    case QUIC_0_RTT_PROTECTED:      return "0P";
    case QUIC_HANDSHAKE:            return "HS";
    case QUIC_RETRY:                return "R";
    }
    return "INVALID";
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicPacketLogHeader(
    _In_opt_ QUIC_CONNECTION* Connection,
    _In_ BOOLEAN Rx,
    _In_ uint8_t CIDLength,
    _In_ uint64_t PacketNumber,
    _In_ uint16_t PacketLength,
    _In_reads_bytes_(PacketLength)
        const uint8_t * const Packet,
    _In_ uint32_t Version             // Network Byte Order. Used for Short Headers
    )
{
    const QUIC_HEADER_INVARIANT* Invariant = (QUIC_HEADER_INVARIANT*)Packet;
    uint16_t Offset;

    if (Invariant->IsLongHeader) {

        uint8_t DestCidLen = Invariant->LONG_HDR.DestCidLength;
        const uint8_t* DestCid = Invariant->LONG_HDR.DestCid;
        uint8_t SourceCidLen = *(DestCid + DestCidLen);
        const uint8_t* SourceCid = DestCid + sizeof(uint8_t) + DestCidLen;

        Offset = sizeof(QUIC_HEADER_INVARIANT) + sizeof(uint8_t) + DestCidLen + SourceCidLen;

        switch (Invariant->LONG_HDR.Version) {
        case QUIC_VERSION_VER_NEG: {
            QuicTraceLogVerbose(
                "[%c][%cX][-] VerNeg DestCid:%s SrcCid:%s (Payload %lu bytes)",
                PtkConnPre(Connection),
                PktRxPre(Rx),
                QuicCidBufToStr(DestCid, DestCidLen).Buffer,
                QuicCidBufToStr(SourceCid, SourceCidLen).Buffer,
                PacketLength - Offset);

            while (Offset < PacketLength) {
                QuicTraceLogVerbose(
                    "[%c][%cX][-]   Ver:0x%x",
                    PtkConnPre(Connection),
                    PktRxPre(Rx),
                    *(uint32_t*)(Packet + Offset));
                Offset += sizeof(uint32_t);
            }
            break;
        }

        case QUIC_VERSION_DRAFT_24:
        case QUIC_VERSION_MS_1: {
            const QUIC_LONG_HEADER_V1 * const LongHdr =
                (const QUIC_LONG_HEADER_V1 * const)Packet;

            QUIC_VAR_INT TokenLength;
            QUIC_VAR_INT Length;

            if (LongHdr->Type == QUIC_INITIAL) {
                if (!QuicVarIntDecode(
                        PacketLength,
                        Packet,
                        &Offset,
                        &TokenLength)) {
                    break;
                }
                Offset += (uint16_t)TokenLength;

            } else if (LongHdr->Type == QUIC_RETRY) {

                uint8_t OrigDestCidLen = *(SourceCid + SourceCidLen);
                const uint8_t* OrigDestCid = SourceCid + sizeof(uint8_t) + SourceCidLen;
                Offset += sizeof(uint8_t) + OrigDestCidLen;

                QuicTraceLogVerbose(
                    "[%c][%cX][-] LH Ver:0x%x DestCid:%s SrcCid:%s Type:R OrigDestCid:%s (Token %hu bytes)",
                    PtkConnPre(Connection),
                    PktRxPre(Rx),
                    LongHdr->Version,
                    QuicCidBufToStr(DestCid, DestCidLen).Buffer,
                    QuicCidBufToStr(SourceCid, SourceCidLen).Buffer,
                    QuicCidBufToStr(OrigDestCid, OrigDestCidLen).Buffer,
                    PacketLength - Offset);
                break;

            } else {
                TokenLength = UINT64_MAX;
            }

            if (!QuicVarIntDecode(
                    PacketLength,
                    Packet,
                    &Offset,
                    &Length)) {
                break;
            }

            if (LongHdr->Type == QUIC_INITIAL) {
                QuicTraceLogVerbose(
                    "[%c][%cX][%llu] LH Ver:0x%x DestCid:%s SrcCid:%s Type:%s (Token %hu bytes) (Payload %hu bytes) (PktNum %hu bytes)",
                    PtkConnPre(Connection),
                    PktRxPre(Rx),
                    PacketNumber,
                    LongHdr->Version,
                    QuicCidBufToStr(DestCid, DestCidLen).Buffer,
                    QuicCidBufToStr(SourceCid, SourceCidLen).Buffer,
                    QuicLongHeaderTypeToString(LongHdr->Type),
                    (uint16_t)TokenLength,
                    (uint16_t)Length,
                    LongHdr->PnLength + 1);
            } else {
                QuicTraceLogVerbose(
                    "[%c][%cX][%llu] LH Ver:0x%x DestCid:%s SrcCid:%s Type:%s (Payload %hu bytes) (PktNum %hu bytes)",
                    PtkConnPre(Connection),
                    PktRxPre(Rx),
                    PacketNumber,
                    LongHdr->Version,
                    QuicCidBufToStr(DestCid, DestCidLen).Buffer,
                    QuicCidBufToStr(SourceCid, SourceCidLen).Buffer,
                    QuicLongHeaderTypeToString(LongHdr->Type),
                    (uint16_t)Length,
                    LongHdr->PnLength + 1);
            }
            break;
        }

        default:
            QuicTraceLogVerbose(
                "[%c][%cX][%llu] LH Ver:[UNSUPPORTED,0x%x] DestCid:%s SrcCid:%s",
                PtkConnPre(Connection),
                PktRxPre(Rx),
                PacketNumber,
                Invariant->LONG_HDR.Version,
                QuicCidBufToStr(DestCid, DestCidLen).Buffer,
                QuicCidBufToStr(SourceCid, SourceCidLen).Buffer);
            break;
        }

    } else {

        uint8_t DestCidLen = CIDLength;
        const uint8_t* DestCid = Invariant->SHORT_HDR.DestCid;

        switch (Version) {
        case QUIC_VERSION_DRAFT_24:
        case QUIC_VERSION_MS_1: {
            const QUIC_SHORT_HEADER_V1 * const Header =
                (const QUIC_SHORT_HEADER_V1 * const)Packet;

            Offset = sizeof(QUIC_SHORT_HEADER_V1) + DestCidLen;

            QuicTraceLogVerbose(
                "[%c][%cX][%llu] SH DestCid:%s KP:%hu SB:%hu (Payload %hu bytes)",
                PtkConnPre(Connection),
                PktRxPre(Rx),
                PacketNumber,
                QuicCidBufToStr(DestCid, DestCidLen).Buffer,
                Header->KeyPhase,
                Header->SpinBit,
                PacketLength - Offset);
            break;
        }

        default:
            QUIC_FRE_ASSERT(FALSE);
            break;
        }
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicPacketLogDrop(
    _In_ const void* Owner, // Binding or Connection depending on state
    _In_ const QUIC_RECV_PACKET* Packet,
    _In_z_ const char* Reason
    )
{
    const QUIC_RECV_DATAGRAM* Datagram =
        QuicDataPathRecvPacketToRecvDatagram(Packet);

    if (Packet->AssignedToConnection) {
        InterlockedIncrement64((int64_t*) &((QUIC_CONNECTION*)Owner)->Stats.Recv.DroppedPackets);
        QuicTraceEvent(ConnDropPacket,
            Owner,
            Packet->PacketNumberSet ? UINT64_MAX : Packet->PacketNumber,
            LOG_ADDR_LEN(Datagram->Tuple->LocalAddress),
            LOG_ADDR_LEN(Datagram->Tuple->RemoteAddress),
            (uint8_t*)&Datagram->Tuple->LocalAddress,
            (uint8_t*)&Datagram->Tuple->RemoteAddress,
            Reason);
    } else {
        InterlockedIncrement64((int64_t*) &((QUIC_BINDING*)Owner)->Stats.Recv.DroppedPackets);
        QuicTraceEvent(BindingDropPacket,
            Owner,
            Packet->PacketNumberSet ? UINT64_MAX : Packet->PacketNumber,
            LOG_ADDR_LEN(Datagram->Tuple->LocalAddress),
            LOG_ADDR_LEN(Datagram->Tuple->RemoteAddress),
            (uint8_t*)&Datagram->Tuple->LocalAddress,
            (uint8_t*)&Datagram->Tuple->RemoteAddress,
            Reason);
    }
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void
QuicPacketLogDropWithValue(
    _In_ const void* Owner, // Binding or Connection depending on state
    _In_ const QUIC_RECV_PACKET* Packet,
    _In_z_ const char* Reason,
    _In_ uint64_t Value
    )
{
    const QUIC_RECV_DATAGRAM* Datagram =
        QuicDataPathRecvPacketToRecvDatagram(Packet);

    if (Packet->AssignedToConnection) {
        InterlockedIncrement64((int64_t*) & ((QUIC_CONNECTION*)Owner)->Stats.Recv.DroppedPackets);
        QuicTraceEvent(ConnDropPacketEx,
            Owner,
            Packet->PacketNumberSet ? UINT64_MAX : Packet->PacketNumber,
            Value,
            LOG_ADDR_LEN(Datagram->Tuple->LocalAddress),
            LOG_ADDR_LEN(Datagram->Tuple->RemoteAddress),
            (uint8_t*)&Datagram->Tuple->LocalAddress,
            (uint8_t*)&Datagram->Tuple->RemoteAddress,
            Reason);
    } else {
        InterlockedIncrement64((int64_t*) &((QUIC_BINDING*)Owner)->Stats.Recv.DroppedPackets);
        QuicTraceEvent(BindingDropPacketEx,
            Owner,
            Packet->PacketNumberSet ? UINT64_MAX : Packet->PacketNumber,
            Value,
            LOG_ADDR_LEN(Datagram->Tuple->LocalAddress),
            LOG_ADDR_LEN(Datagram->Tuple->RemoteAddress),
            (uint8_t*)&Datagram->Tuple->LocalAddress,
            (uint8_t*)&Datagram->Tuple->RemoteAddress,
            Reason);
    }
}