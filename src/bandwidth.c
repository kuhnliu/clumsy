// bandwidth cap module
#include <stdlib.h>
#include <Windows.h>
#include <stdint.h>

#include "iup.h"
#include "common.h"

#define NAME "bandwidth"
#define BANDWIDTH_MIN  "0"
#define BANDWIDTH_MAX  "99999"
#define BANDWIDTH_DEFAULT 100
#define QUEUESIZE_MIN  "0"
#define QUEUESIZE_MAX  "99999"
#define QUEUESIZE_DEFAULT 100

#define TB_LINK_MTU 1500
#define TB_DEFAULT_BUSRT_TIME 25

#define TB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define TB_MIN(a, b) ((a) < (b) ? (a) : (b))

//---------------------------------------------------------------------
// token bucket
//---------------------------------------------------------------------
typedef struct TokenBucket {
    // Parameters
    int32_t avgRate;
    int32_t burstSize;
    uint32_t busrtTime;

    // Varaibles
    int32_t tokens;
    uint32_t lastTime;
} CTokenBucket;

typedef struct {
    PacketNode queueHeadNode;
    PacketNode queueTailNode;
    PacketNode *queueHead;
    PacketNode *queueTail;
    int queueSizeInBytes;
    CTokenBucket *tokenBucket;
} CRateLimiter;

static CRateLimiter inboundRateLimiter = {0}, outboundRateLimiter = {0};

CTokenBucket* token_bucket_new(int32_t avgRate, uint32_t burstTime);

void token_bucket_delete(CTokenBucket *tb);

void token_bucket_reset(CTokenBucket *tb, int32_t avgRate, uint32_t burstTime);

// call when packet arrives, count is the packet size in bytes
int token_bucket_consume(CTokenBucket *tb, int32_t avgRate, int32_t tokens, uint32_t now);

//---------------------------------------------------------------------
// configuration
//---------------------------------------------------------------------
static Ihandle *inboundCheckbox, *outboundCheckbox, *bandwidthInput, *queueSizeInput, *drainQueueCheckbox;

static volatile short bandwidthEnabled = 0,
    bandwidthInbound = 1, bandwidthOutbound = 1;
static volatile short maxQueueSizeInKBytes = 0;

static volatile LONG bandwidthLimit = BANDWIDTH_DEFAULT; 
static volatile short drainQueueInFull = 1;

static INLINE_FUNCTION short isQueueEmpty(CRateLimiter *rateLimiter) {
    short ret = rateLimiter->queueHead->next == rateLimiter->queueTail;
    if (ret) assert(rateLimiter->queueSizeInBytes == 0);
    return ret;
}

static Ihandle* bandwidthSetupUI() {
    Ihandle *bandwidthControlsBox = IupHbox(
        IupLabel("QueueSize(KB):"),
        queueSizeInput = IupText(NULL),
        drainQueueCheckbox = IupToggle("DrainQueue", NULL),
        inboundCheckbox = IupToggle("Inbound", NULL),
        outboundCheckbox = IupToggle("Outbound", NULL),
        IupLabel("Limit(KB/s):"),
        bandwidthInput = IupText(NULL),
        NULL
    );

    IupSetAttribute(bandwidthInput, "VISIBLECOLUMNS", "4");
    IupSetAttribute(bandwidthInput, "VALUE", STR(BANDWIDTH_DEFAULT));
    IupSetCallback(bandwidthInput, "VALUECHANGED_CB", uiSyncInt32);
    IupSetAttribute(bandwidthInput, SYNCED_VALUE, (char*)&bandwidthLimit);
    IupSetAttribute(bandwidthInput, INTEGER_MAX, BANDWIDTH_MAX);
    IupSetAttribute(bandwidthInput, INTEGER_MIN, BANDWIDTH_MIN);
    IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&bandwidthInbound);
    IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&bandwidthOutbound);
    IupSetCallback(drainQueueCheckbox, "ACTION", (Icallback)uiSyncToggle);
    IupSetAttribute(drainQueueCheckbox, SYNCED_VALUE, (char*)&drainQueueInFull);
    IupSetAttribute(queueSizeInput, "VISIBLECOLUMNS", "3");
    IupSetAttribute(queueSizeInput, "VALUE", STR(QUEUESIZE_DEFAULT));
    IupSetCallback(queueSizeInput, "VALUECHANGED_CB", uiSyncInt32);
    IupSetAttribute(queueSizeInput, SYNCED_VALUE, (char*)&maxQueueSizeInKBytes);
    IupSetAttribute(queueSizeInput, INTEGER_MAX, QUEUESIZE_MAX);
    IupSetAttribute(queueSizeInput, INTEGER_MIN, QUEUESIZE_MIN);

    // enable by default to avoid confusing
    IupSetAttribute(drainQueueCheckbox, "VALUE", "ON");
    IupSetAttribute(inboundCheckbox, "VALUE", "ON");
    IupSetAttribute(outboundCheckbox, "VALUE", "ON");

    if (parameterized) {
        setFromParameter(inboundCheckbox, "VALUE", NAME"-inbound");
        setFromParameter(outboundCheckbox, "VALUE", NAME"-outbound");
        setFromParameter(bandwidthInput, "VALUE", NAME"-bandwidth");
        setFromParameter(drainQueueCheckbox, "VALUE", NAME"-drainqueue");
    }

    return bandwidthControlsBox;
}

static void initRateLimiter(CRateLimiter *rateLimiter) {
    rateLimiter->queueHead = &rateLimiter->queueHeadNode;
    rateLimiter->queueTail = &rateLimiter->queueTailNode;

    if (rateLimiter->queueHead->next == NULL && rateLimiter->queueTail->next == NULL) {
        rateLimiter->queueHead->next = rateLimiter->queueTail;
        rateLimiter->queueTail->prev = rateLimiter->queueHead;
        rateLimiter->queueSizeInBytes = 0;
    } else {
        assert(isQueueEmpty(rateLimiter));
    }

	if (rateLimiter->tokenBucket) token_bucket_delete(rateLimiter->tokenBucket);
	rateLimiter->tokenBucket = token_bucket_new(bandwidthLimit * 1024, TB_DEFAULT_BUSRT_TIME);
}

static int uninitRateLimiter(CRateLimiter *rateLimiter, PacketNode *head, PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    UNREFERENCED_PARAMETER(head);
    // flush all buffered packets
    int packetCnt = 0;
    while(!isQueueEmpty(rateLimiter)) {
        rateLimiter->queueSizeInBytes -= rateLimiter->queueTail->prev->packetLen;
        insertAfter(popNode(rateLimiter->queueTail->prev), oldLast);
        ++packetCnt;
    }

    if (rateLimiter->tokenBucket) {
        token_bucket_delete(rateLimiter->tokenBucket);
        rateLimiter->tokenBucket = NULL;
    }

    return packetCnt;
}

static void bandwidthStartUp() {
	initRateLimiter(&inboundRateLimiter);
    initRateLimiter(&outboundRateLimiter);
    startTimePeriod();
    LOG("bandwidth enabled");
}

static void bandwidthCloseDown(PacketNode *head, PacketNode *tail) {
    int packetCnt = 0;
    packetCnt = uninitRateLimiter(&inboundRateLimiter, head, tail);
    LOG("Closing down bandwidth, flushing inbound %d packets", packetCnt);
    packetCnt = uninitRateLimiter(&outboundRateLimiter, head, tail);
    LOG("Closing down bandwidth, flushing outbound %d packets", packetCnt);
    endTimePeriod();
    LOG("bandwidth disabled");
}


//---------------------------------------------------------------------
// process
//---------------------------------------------------------------------
static int rateLimiterProcess(CRateLimiter *rateLimiter, PacketNode *head, PacketNode* tail) {
    PacketNode *pac;
    DWORD now_ts = timeGetTime();
    int limit = bandwidthLimit * 1024;

    while (!isQueueEmpty(rateLimiter)) {
        pac = rateLimiter->queueTail->prev;
        // chance in range of [0, 10000]
        if (token_bucket_consume(rateLimiter->tokenBucket, limit, pac->packetLen, now_ts) == 0) {
            break;
        }
        rateLimiter->queueSizeInBytes -= pac->packetLen;
        insertAfter(popNode(pac), head);
    }

    int dropped = 0;
    int targetQueueSize = maxQueueSizeInKBytes * 1024;
    if (rateLimiter->queueSizeInBytes > targetQueueSize && !isQueueEmpty(rateLimiter)) {
        if (drainQueueInFull) {
            targetQueueSize = targetQueueSize / 3;
        }

        do {
            pac = rateLimiter->queueHead->next;
            LOG("dropped with bandwidth %dKB/s, direction %s",
                (int)bandwidthLimit, pac->addr.Outbound ? "OUTBOUND" : "INBOUND");
            rateLimiter->queueSizeInBytes -= pac->packetLen;
            freeNode(popNode(pac));
            ++dropped;
        } while (rateLimiter->queueSizeInBytes > targetQueueSize && !isQueueEmpty(rateLimiter));
    }

    assert(rateLimiter->queueSizeInBytes >= 0);

    return dropped;
}

static short bandwidthProcess(PacketNode *head, PacketNode* tail) {
	if (inboundRateLimiter.tokenBucket == NULL || outboundRateLimiter.tokenBucket == NULL) {
		return 0;
	}

    PacketNode *pac = tail->prev;
    while (pac != head) {
        if (checkDirection(pac->addr.Outbound, bandwidthInbound, bandwidthOutbound)) {
            if (pac->addr.Outbound) {
                outboundRateLimiter.queueSizeInBytes += pac->packetLen;
                insertAfter(popNode(pac), outboundRateLimiter.queueHead);
            } else {
                inboundRateLimiter.queueSizeInBytes += pac->packetLen;
                insertAfter(popNode(pac), inboundRateLimiter.queueHead);
            }
            pac = tail->prev;
        } else {
            pac = pac->prev;
        }
    }

    int dropped;
    dropped = rateLimiterProcess(&inboundRateLimiter, head, tail);
    dropped += rateLimiterProcess(&outboundRateLimiter, head, tail);

    return dropped > 0 || !isQueueEmpty(&inboundRateLimiter) || !isQueueEmpty(&outboundRateLimiter);
}


//---------------------------------------------------------------------
// module
//---------------------------------------------------------------------
Module bandwidthModule = {
    "Bandwidth",
    NAME,
    (short*)&bandwidthEnabled,
    bandwidthSetupUI,
    bandwidthStartUp,
    bandwidthCloseDown,
    bandwidthProcess,
    // runtime fields
    0, 0, NULL
};



//---------------------------------------------------------------------
// create new TokenBucket
//---------------------------------------------------------------------
CTokenBucket* token_bucket_new(int32_t avgRate, uint32_t burstTime)
{
	CTokenBucket *tb = (CTokenBucket*)malloc(sizeof(CTokenBucket));
	assert(tb);
	token_bucket_reset(tb, avgRate, burstTime);
	return tb;
}


//---------------------------------------------------------------------
// delete rate
//---------------------------------------------------------------------
void token_bucket_delete(CTokenBucket *tb)
{
	if (tb) {
		free(tb);
	}
}


//---------------------------------------------------------------------
// reset token bucket
//---------------------------------------------------------------------
void token_bucket_reset(CTokenBucket *tb, int32_t avgRate, uint32_t burstTime)
{
    tb->avgRate = avgRate;
    tb->busrtTime = burstTime;
    tb->burstSize = TB_MAX((avgRate * burstTime + 500) / 1000, TB_LINK_MTU);
    tb->tokens = tb->burstSize;
    tb->lastTime = 0;
}


//---------------------------------------------------------------------
// consume token
//---------------------------------------------------------------------
int token_bucket_consume(CTokenBucket *tb, int32_t avgRate, int32_t tokens, uint32_t now)
{
    uint32_t elapsed = now - tb->lastTime;

    if (tb->avgRate != avgRate) {
        token_bucket_reset(tb, avgRate, tb->busrtTime);
    }

    tb->lastTime = now;
    tb->tokens += (elapsed * tb->avgRate + 500) / 1000;
    tb->tokens = TB_MIN(tb->tokens, tb->burstSize);

    if (tb->tokens < tokens)
    {
        return 0;
    }

    tb->tokens -= tokens;

    return 1;
}

