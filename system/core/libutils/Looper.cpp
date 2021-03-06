//
// Copyright 2010 The Android Open Source Project
//
// A looper implementation based on epoll().
//
#define LOG_TAG "Looper"

//#define LOG_NDEBUG 0

// Debugs poll and wake interactions.
#define DEBUG_POLL_AND_WAKE 0

// Debugs callback registration and invocation.
#define DEBUG_CALLBACKS 0

#include <utils/Looper.h>

#include <sys/eventfd.h>
#include <cinttypes>

namespace android {

// --- WeakMessageHandler ---

WeakMessageHandler::WeakMessageHandler(const wp<MessageHandler>& handler) :
        mHandler(handler) {
}

WeakMessageHandler::~WeakMessageHandler() {
}

void WeakMessageHandler::handleMessage(const Message& message) {
    sp<MessageHandler> handler = mHandler.promote();
    if (handler != nullptr) {
        handler->handleMessage(message);
    }
}


// --- SimpleLooperCallback ---

SimpleLooperCallback::SimpleLooperCallback(Looper_callbackFunc callback) :
        mCallback(callback) {
}

SimpleLooperCallback::~SimpleLooperCallback() {
}

int SimpleLooperCallback::handleEvent(int fd, int events, void* data) {
    return mCallback(fd, events, data);
}


// --- Looper ---

// Maximum number of file descriptors for which to retrieve poll events each iteration.
static const int EPOLL_MAX_EVENTS = 16;

static pthread_once_t gTLSOnce = PTHREAD_ONCE_INIT;
static pthread_key_t gTLSKey = 0;

// dg2: 构造函数.
Looper::Looper(bool allowNonCallbacks)
    : mAllowNonCallbacks(allowNonCallbacks),
      mSendingMessage(false),
      mPolling(false),
      mEpollRebuildRequired(false),
      mNextRequestSeq(0),
      mResponseIndex(0),
      mNextMessageUptime(LLONG_MAX) {
	// dg2: eventFd()系统调用获取了一个mWakeEventFd作为后续epoll()用于唤醒的FD.
    mWakeEventFd.reset(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    LOG_ALWAYS_FATAL_IF(mWakeEventFd.get() < 0, "Could not make wake event fd: %s", strerror(errno));

    AutoMutex _l(mLock);
    rebuildEpollLocked();
}

Looper::~Looper() {
}

void Looper::initTLSKey() {
    int error = pthread_key_create(&gTLSKey, threadDestructor);
    LOG_ALWAYS_FATAL_IF(error != 0, "Could not allocate TLS key: %s", strerror(error));
}

void Looper::threadDestructor(void *st) {
    Looper* const self = static_cast<Looper*>(st);
    if (self != nullptr) {
        self->decStrong((void*)threadDestructor);
    }
}

void Looper::setForThread(const sp<Looper>& looper) {
    sp<Looper> old = getForThread(); // also has side-effect of initializing TLS

    if (looper != nullptr) {
        looper->incStrong((void*)threadDestructor);
    }

    pthread_setspecific(gTLSKey, looper.get());

    if (old != nullptr) {
        old->decStrong((void*)threadDestructor);
    }
}

sp<Looper> Looper::getForThread() {
    int result = pthread_once(& gTLSOnce, initTLSKey);
    LOG_ALWAYS_FATAL_IF(result != 0, "pthread_once failed");

    return (Looper*)pthread_getspecific(gTLSKey);
}

sp<Looper> Looper::prepare(int opts) {
    bool allowNonCallbacks = opts & PREPARE_ALLOW_NON_CALLBACKS;
    sp<Looper> looper = Looper::getForThread();
    if (looper == nullptr) {
        looper = new Looper(allowNonCallbacks);
        Looper::setForThread(looper);
    }
    if (looper->getAllowNonCallbacks() != allowNonCallbacks) {
        ALOGW("Looper already prepared for this thread with a different value for the "
                "LOOPER_PREPARE_ALLOW_NON_CALLBACKS option.");
    }
    return looper;
}

bool Looper::getAllowNonCallbacks() const {
    return mAllowNonCallbacks;
}

// dg2: 构建 native 层 Looper对象, 注册 epoll.
void Looper::rebuildEpollLocked() {
    // Close old epoll instance if we have one.
    if (mEpollFd >= 0) {
#if DEBUG_CALLBACKS
        ALOGD("%p ~ rebuildEpollLocked - rebuilding epoll set", this);
#endif
        mEpollFd.reset();
    }

    // Allocate the new epoll instance and register the wake pipe.
	// dg2: 创建新的epoll对象
    mEpollFd.reset(epoll_create1(EPOLL_CLOEXEC));
    LOG_ALWAYS_FATAL_IF(mEpollFd < 0, "Could not create epoll instance: %s", strerror(errno));

    struct epoll_event eventItem;
    memset(& eventItem, 0, sizeof(epoll_event)); // zero out unused members of data field union
    eventItem.events = EPOLLIN;
    eventItem.data.fd = mWakeEventFd.get();
	// dg2: 调用epoll_ctl()来将之前创建的mWakeEventFd与eventItem注册到epoll。
    int result = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, mWakeEventFd.get(), &eventItem);
    LOG_ALWAYS_FATAL_IF(result != 0, "Could not add wake event fd to epoll instance: %s",
                        strerror(errno));

    for (size_t i = 0; i < mRequests.size(); i++) {
        const Request& request = mRequests.valueAt(i);
        struct epoll_event eventItem;
        request.initEventItem(&eventItem);

        int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, request.fd, &eventItem);
        if (epollResult < 0) {
            ALOGE("Error adding epoll events for fd %d while rebuilding epoll set: %s",
                  request.fd, strerror(errno));
        }
    }
}

void Looper::scheduleEpollRebuildLocked() {
    if (!mEpollRebuildRequired) {
#if DEBUG_CALLBACKS
        ALOGD("%p ~ scheduleEpollRebuildLocked - scheduling epoll set rebuild", this);
#endif
        mEpollRebuildRequired = true;
        wake();
    }
}

// dg2: 卡死堆栈中经常看到的 nativePollOnce() 最终会调用到这里.  核心.
// 从mResponse容器中取出了一个response并把他的内容放入了传入的地址参数中返回。
int Looper::pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    int result = 0;
    for (;;) {
        while (mResponseIndex < mResponses.size()) {
            const Response& response = mResponses.itemAt(mResponseIndex++);
            int ident = response.request.ident;
            if (ident >= 0) {
                int fd = response.request.fd;
                int events = response.events;
                void* data = response.request.data;
#if DEBUG_POLL_AND_WAKE
                ALOGD("%p ~ pollOnce - returning signalled identifier %d: "
                        "fd=%d, events=0x%x, data=%p",
                        this, ident, fd, events, data);
#endif
                if (outFd != nullptr) *outFd = fd;
                if (outEvents != nullptr) *outEvents = events;
                if (outData != nullptr) *outData = data;
                return ident;
            }
        }

        if (result != 0) {
#if DEBUG_POLL_AND_WAKE
            ALOGD("%p ~ pollOnce - returning result %d", this, result);
#endif
            if (outFd != nullptr) *outFd = 0;
            if (outEvents != nullptr) *outEvents = 0;
            if (outData != nullptr) *outData = nullptr;
            return result;
        }

		// dg2: 核心实现在这里.
        result = pollInner(timeoutMillis);
    }
}

// dg2: native消息机制的核心. 核心.
int Looper::pollInner(int timeoutMillis) {
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ pollOnce - waiting: timeoutMillis=%d", this, timeoutMillis);
#endif

    // Adjust the timeout based on when the next message is due.
    if (timeoutMillis != 0 && mNextMessageUptime != LLONG_MAX) {
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        int messageTimeoutMillis = toMillisecondTimeoutDelay(now, mNextMessageUptime);
        if (messageTimeoutMillis >= 0
                && (timeoutMillis < 0 || messageTimeoutMillis < timeoutMillis)) {
            timeoutMillis = messageTimeoutMillis;
        }
#if DEBUG_POLL_AND_WAKE
        ALOGD("%p ~ pollOnce - next message in %" PRId64 "ns, adjusted timeout: timeoutMillis=%d",
                this, mNextMessageUptime - now, timeoutMillis);
#endif
    }

    // Poll.
    int result = POLL_WAKE;
    mResponses.clear();
    mResponseIndex = 0;

    // We are about to idle.
    mPolling = true;

    struct epoll_event eventItems[EPOLL_MAX_EVENTS];
	// dg2: 这是一个阻塞调用。 执行这一句后, 本进程进入等待. 核心.
	// 当注册的fd有消息, 或超时, 再继续执行后面的逻辑. timeoutMillis 为指定的超时时间.
    int eventCount = epoll_wait(mEpollFd.get(), eventItems, EPOLL_MAX_EVENTS, timeoutMillis);

    // No longer idling.
    mPolling = false;

    // Acquire lock.
    mLock.lock();

    // Rebuild epoll set if needed.
    if (mEpollRebuildRequired) {
        mEpollRebuildRequired = false;
        rebuildEpollLocked();
        goto Done;
    }

    // Check for poll error.
	// dg2: 检查轮询错误.
    if (eventCount < 0) {
        if (errno == EINTR) {
            goto Done;
        }
        ALOGW("Poll failed with an unexpected error: %s", strerror(errno));
        result = POLL_ERROR;
        goto Done;
    }

    // Check for poll timeout.
	// dg2: 检查轮询超时. 如果超时, 跳转到 Done处继续处理.
    if (eventCount == 0) {
#if DEBUG_POLL_AND_WAKE
        ALOGD("%p ~ pollOnce - timeout", this);
#endif
        result = POLL_TIMEOUT;
        goto Done;
    }

    // Handle all events.
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ pollOnce - handling events from %d fds", this, eventCount);
#endif

	// dg2: 遍历返回的事件, 一个一个进行处理.
    for (int i = 0; i < eventCount; i++) {
        int fd = eventItems[i].data.fd;
        uint32_t epollEvents = eventItems[i].events;
		// dg2: 判断fd是不是用于唤醒的 mWakeEventFd.
        if (fd == mWakeEventFd.get()) {
			// dg2: 判断 events 标志是否正确.
            if (epollEvents & EPOLLIN) {
				// dg2: 如果对应的fd为唤醒专用的mWakeEventFd，执行awoken()函数清空管道，
				// 因为这个事件的作用只是为了唤醒Looper对新消息进行处理。
                awoken();
            } else {
                ALOGW("Ignoring unexpected epoll events 0x%x on wake event fd.", epollEvents);
            }
        } else {
			// dg2: 如果不是mWakeEventFd，说明是我们之前通过addFd()函数添加的自定义fd，
			// 我们需要对这个event进行处理，处理函数为pushResponse().
			// 之前在addFd()中, 以fd为key，Request为value, 生成 mRequest映射表。此处取出处理即可.
			// request信息包含 callback, 也就是 NativeInputEventReceiver对象。
            ssize_t requestIndex = mRequests.indexOfKey(fd);
            if (requestIndex >= 0) {
                int events = 0;
                if (epollEvents & EPOLLIN) events |= EVENT_INPUT;
                if (epollEvents & EPOLLOUT) events |= EVENT_OUTPUT;
                if (epollEvents & EPOLLERR) events |= EVENT_ERROR;
                if (epollEvents & EPOLLHUP) events |= EVENT_HANGUP;
				// dg2: 将request对象包装成了一个response，然后存入了mResponses中等待后面的处理。
                pushResponse(events, mRequests.valueAt(requestIndex));
            } else {
                ALOGW("Ignoring unexpected epoll events 0x%x on fd %d that is "
                        "no longer registered.", epollEvents, fd);
            }
        }
    }

Done: ;

    // Invoke pending message callbacks.
	// dg2: 执行MessageEnvelop消息的处理. MessageEnvelop优先于fd引起的request的处理。
    mNextMessageUptime = LLONG_MAX;
    while (mMessageEnvelopes.size() != 0) {
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        const MessageEnvelope& messageEnvelope = mMessageEnvelopes.itemAt(0);
		// dg2: 如果msg已经到时间, 则马上处理.
        if (messageEnvelope.uptime <= now) {
            // Remove the envelope from the list.
            // We keep a strong reference to the handler until the call to handleMessage
            // finishes.  Then we drop it so that the handler can be deleted *before*
            // we reacquire our lock.
            { // obtain handler
                sp<MessageHandler> handler = messageEnvelope.handler;
                Message message = messageEnvelope.message;
                mMessageEnvelopes.removeAt(0);
                mSendingMessage = true;
                mLock.unlock();

#if DEBUG_POLL_AND_WAKE || DEBUG_CALLBACKS
                ALOGD("%p ~ pollOnce - sending message: handler=%p, what=%d",
                        this, handler.get(), message.what);
#endif
				// dg2: 调用 handler的handleMessage()来处理消息。
                handler->handleMessage(message);
            } // release handler

            mLock.lock();
            mSendingMessage = false;
            result = POLL_CALLBACK;
        } else {
            // The last message left at the head of the queue determines the next wakeup time.
            mNextMessageUptime = messageEnvelope.uptime;
            break;
        }
    }

    // Release lock.
    mLock.unlock();

    // Invoke all response callbacks.
	// dg2: 遍历所有的 response 对象.
    for (size_t i = 0; i < mResponses.size(); i++) {
		// dg2: 取出之前注册的request对象的信息.
        Response& response = mResponses.editItemAt(i);
        if (response.request.ident == POLL_CALLBACK) {
            int fd = response.request.fd;
            int events = response.events;
            void* data = response.request.data;
#if DEBUG_POLL_AND_WAKE || DEBUG_CALLBACKS
            ALOGD("%p ~ pollOnce - invoking fd event callback %p: fd=%d, events=0x%x, data=%p",
                    this, response.request.callback.get(), fd, events, data);
#endif
            // Invoke the callback.  Note that the file descriptor may be closed by
            // the callback (and potentially even reused) before the function returns so
            // we need to be a little careful when removing the file descriptor afterwards.

			// dg2: 调用了request.callback->handleEvent()方法进行回调.
			// callback 即 NativeInputEventReceiver对象.
            int callbackResult = response.request.callback->handleEvent(fd, events, data);
			// dg2: 如果该回调返回0，则取消这个fd的注册。
            if (callbackResult == 0) {
                removeFd(fd, response.request.seq);
            }

            // Clear the callback reference in the response structure promptly because we
            // will not clear the response vector itself until the next poll.
            response.request.callback.clear();
            result = POLL_CALLBACK;
        }
    }
    return result;
}

int Looper::pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    if (timeoutMillis <= 0) {
        int result;
        do {
            result = pollOnce(timeoutMillis, outFd, outEvents, outData);
        } while (result == POLL_CALLBACK);
        return result;
    } else {
        nsecs_t endTime = systemTime(SYSTEM_TIME_MONOTONIC)
                + milliseconds_to_nanoseconds(timeoutMillis);

        for (;;) {
            int result = pollOnce(timeoutMillis, outFd, outEvents, outData);
            if (result != POLL_CALLBACK) {
                return result;
            }

            nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
            timeoutMillis = toMillisecondTimeoutDelay(now, endTime);
            if (timeoutMillis == 0) {
                return POLL_TIMEOUT;
            }
        }
    }
}

// dg2: 向FD写入 1, 以唤醒消息队列.
// 由于此FD已经注册了epoll监听, 所以一旦有新内容, 
// NativePollOnce()中的epoll_wait()调用就会返回，就起到了唤醒队列的作用。
void Looper::wake() {
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ wake", this);
#endif

    uint64_t inc = 1;
    ssize_t nWrite = TEMP_FAILURE_RETRY(write(mWakeEventFd.get(), &inc, sizeof(uint64_t)));
    if (nWrite != sizeof(uint64_t)) {
        if (errno != EAGAIN) {
            LOG_ALWAYS_FATAL("Could not write wake signal to fd %d (returned %zd): %s",
                             mWakeEventFd.get(), nWrite, strerror(errno));
        }
    }
}

// dg2: 从FD中读取数据并清零.
void Looper::awoken() {
#if DEBUG_POLL_AND_WAKE
    ALOGD("%p ~ awoken", this);
#endif

    uint64_t counter;
    TEMP_FAILURE_RETRY(read(mWakeEventFd.get(), &counter, sizeof(uint64_t)));
}

void Looper::pushResponse(int events, const Request& request) {
    Response response;
    response.events = events;
    response.request = request;
    mResponses.push(response);
}

int Looper::addFd(int fd, int ident, int events, Looper_callbackFunc callback, void* data) {
    return addFd(fd, ident, events, callback ? new SimpleLooperCallback(callback) : nullptr, data);
}

// dg2: 注册自 fd. 保存 request并使用epoll_ctl系统调用注册fd的监听。关键.
int Looper::addFd(int fd, int ident, int events, const sp<LooperCallback>& callback, void* data) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ addFd - fd=%d, ident=%d, events=0x%x, callback=%p, data=%p", this, fd, ident,
            events, callback.get(), data);
#endif

    if (!callback.get()) {
        if (! mAllowNonCallbacks) {
            ALOGE("Invalid attempt to set NULL callback but not allowed for this looper.");
            return -1;
        }

        if (ident < 0) {
            ALOGE("Invalid attempt to set NULL callback with ident < 0.");
            return -1;
        }
    } else {
        ident = POLL_CALLBACK;
    }

    { // acquire lock
        AutoMutex _l(mLock);

		// dg2: 用传入的参数初始化了request对象.
        Request request;
        request.fd = fd;
        request.ident = ident;
        request.events = events;
        request.seq = mNextRequestSeq++;
        request.callback = callback;
        request.data = data;
        if (mNextRequestSeq == -1) mNextRequestSeq = 0; // reserve sequence number -1

        struct epoll_event eventItem;
		// dg2: 由request来初始化注册epoll使用的event.
        request.initEventItem(&eventItem);

		// dg2: 根据mRequests.indexOfKey()方法取出的值来判断fd是否已经注册.
        ssize_t requestIndex = mRequests.indexOfKey(fd);
        if (requestIndex < 0) {
			// dg2: 如果未注册，调用epoll_ctl()注册新监听.
            int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, fd, &eventItem);
            if (epollResult < 0) {
                ALOGE("Error adding epoll events for fd %d: %s", fd, strerror(errno));
                return -1;
            }
			// dg2: 将fd与request存入mRequest.
            mRequests.add(fd, request);
        } else {
			// dg2: 如果已注册，则更新注册。
            int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_MOD, fd, &eventItem);
            if (epollResult < 0) {
                if (errno == ENOENT) {
                    // Tolerate ENOENT because it means that an older file descriptor was
                    // closed before its callback was unregistered and meanwhile a new
                    // file descriptor with the same number has been created and is now
                    // being registered for the first time.  This error may occur naturally
                    // when a callback has the side-effect of closing the file descriptor
                    // before returning and unregistering itself.  Callback sequence number
                    // checks further ensure that the race is benign.
                    //
                    // Unfortunately due to kernel limitations we need to rebuild the epoll
                    // set from scratch because it may contain an old file handle that we are
                    // now unable to remove since its file descriptor is no longer valid.
                    // No such problem would have occurred if we were using the poll system
                    // call instead, but that approach carries others disadvantages.
#if DEBUG_CALLBACKS
                    ALOGD("%p ~ addFd - EPOLL_CTL_MOD failed due to file descriptor "
                            "being recycled, falling back on EPOLL_CTL_ADD: %s",
                            this, strerror(errno));
#endif
                    epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_ADD, fd, &eventItem);
                    if (epollResult < 0) {
                        ALOGE("Error modifying or adding epoll events for fd %d: %s",
                                fd, strerror(errno));
                        return -1;
                    }
                    scheduleEpollRebuildLocked();
                } else {
                    ALOGE("Error modifying epoll events for fd %d: %s", fd, strerror(errno));
                    return -1;
                }
            }
			// dg2: 更新request。
            mRequests.replaceValueAt(requestIndex, request);
        }
    } // release lock
    return 1;
}

int Looper::removeFd(int fd) {
    return removeFd(fd, -1);
}

int Looper::removeFd(int fd, int seq) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ removeFd - fd=%d, seq=%d", this, fd, seq);
#endif

    { // acquire lock
        AutoMutex _l(mLock);
        ssize_t requestIndex = mRequests.indexOfKey(fd);
        if (requestIndex < 0) {
            return 0;
        }

        // Check the sequence number if one was given.
        if (seq != -1 && mRequests.valueAt(requestIndex).seq != seq) {
#if DEBUG_CALLBACKS
            ALOGD("%p ~ removeFd - sequence number mismatch, oldSeq=%d",
                    this, mRequests.valueAt(requestIndex).seq);
#endif
            return 0;
        }

        // Always remove the FD from the request map even if an error occurs while
        // updating the epoll set so that we avoid accidentally leaking callbacks.
        mRequests.removeItemsAt(requestIndex);

        int epollResult = epoll_ctl(mEpollFd.get(), EPOLL_CTL_DEL, fd, nullptr);
        if (epollResult < 0) {
            if (seq != -1 && (errno == EBADF || errno == ENOENT)) {
                // Tolerate EBADF or ENOENT when the sequence number is known because it
                // means that the file descriptor was closed before its callback was
                // unregistered.  This error may occur naturally when a callback has the
                // side-effect of closing the file descriptor before returning and
                // unregistering itself.
                //
                // Unfortunately due to kernel limitations we need to rebuild the epoll
                // set from scratch because it may contain an old file handle that we are
                // now unable to remove since its file descriptor is no longer valid.
                // No such problem would have occurred if we were using the poll system
                // call instead, but that approach carries others disadvantages.
#if DEBUG_CALLBACKS
                ALOGD("%p ~ removeFd - EPOLL_CTL_DEL failed due to file descriptor "
                        "being closed: %s", this, strerror(errno));
#endif
                scheduleEpollRebuildLocked();
            } else {
                // Some other error occurred.  This is really weird because it means
                // our list of callbacks got out of sync with the epoll set somehow.
                // We defensively rebuild the epoll set to avoid getting spurious
                // notifications with nowhere to go.
                ALOGE("Error removing epoll events for fd %d: %s", fd, strerror(errno));
                scheduleEpollRebuildLocked();
                return -1;
            }
        }
    } // release lock
    return 1;
}

void Looper::sendMessage(const sp<MessageHandler>& handler, const Message& message) {
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    sendMessageAtTime(now, handler, message);
}

void Looper::sendMessageDelayed(nsecs_t uptimeDelay, const sp<MessageHandler>& handler,
        const Message& message) {
    nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
    sendMessageAtTime(now + uptimeDelay, handler, message);
}

// dg2: 发送指定时间的msg.
void Looper::sendMessageAtTime(nsecs_t uptime, const sp<MessageHandler>& handler,
        const Message& message) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ sendMessageAtTime - uptime=%" PRId64 ", handler=%p, what=%d",
            this, uptime, handler.get(), message.what);
#endif

    size_t i = 0;
    { // acquire lock
        AutoMutex _l(mLock);

        size_t messageCount = mMessageEnvelopes.size();
        while (i < messageCount && uptime >= mMessageEnvelopes.itemAt(i).uptime) {
            i += 1;
        }

        MessageEnvelope messageEnvelope(uptime, handler, message);
        mMessageEnvelopes.insertAt(messageEnvelope, i, 1);

        // Optimization: If the Looper is currently sending a message, then we can skip
        // the call to wake() because the next thing the Looper will do after processing
        // messages is to decide when the next wakeup time should be.  In fact, it does
        // not even matter whether this code is running on the Looper thread.
        if (mSendingMessage) {
            return;
        }
    } // release lock

    // Wake the poll loop only when we enqueue a new message at the head.
    if (i == 0) {
        wake();
    }
}

void Looper::removeMessages(const sp<MessageHandler>& handler) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ removeMessages - handler=%p", this, handler.get());
#endif

    { // acquire lock
        AutoMutex _l(mLock);

        for (size_t i = mMessageEnvelopes.size(); i != 0; ) {
            const MessageEnvelope& messageEnvelope = mMessageEnvelopes.itemAt(--i);
            if (messageEnvelope.handler == handler) {
                mMessageEnvelopes.removeAt(i);
            }
        }
    } // release lock
}

void Looper::removeMessages(const sp<MessageHandler>& handler, int what) {
#if DEBUG_CALLBACKS
    ALOGD("%p ~ removeMessages - handler=%p, what=%d", this, handler.get(), what);
#endif

    { // acquire lock
        AutoMutex _l(mLock);

        for (size_t i = mMessageEnvelopes.size(); i != 0; ) {
            const MessageEnvelope& messageEnvelope = mMessageEnvelopes.itemAt(--i);
            if (messageEnvelope.handler == handler
                    && messageEnvelope.message.what == what) {
                mMessageEnvelopes.removeAt(i);
            }
        }
    } // release lock
}

bool Looper::isPolling() const {
    return mPolling;
}

void Looper::Request::initEventItem(struct epoll_event* eventItem) const {
    int epollEvents = 0;
    if (events & EVENT_INPUT) epollEvents |= EPOLLIN;
    if (events & EVENT_OUTPUT) epollEvents |= EPOLLOUT;

    memset(eventItem, 0, sizeof(epoll_event)); // zero out unused members of data field union
    eventItem->events = epollEvents;
    eventItem->data.fd = fd;
}

MessageHandler::~MessageHandler() { }

LooperCallback::~LooperCallback() { }

} // namespace android
