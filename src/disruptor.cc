// Start by just creating shared memory and checking multiple processes can read
// and write to it.
// Then get the atomic operations working (add and cas).
// Then implement the algorithm. Option to clear all the memory?

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <memory>
#include <napi.h>

typedef uint64_t sequence_t;

class Disruptor : public Napi::ObjectWrap<Disruptor>
{
public:
    Disruptor(const Napi::CallbackInfo& info);
    ~Disruptor();

    static void Initialize(Napi::Env env, Napi::Object exports);

    // Unmap the shared memory. Don't access it again from this process!
    Napi::Value Release(const Napi::CallbackInfo& info);

    // Return unconsumed values for a consumer
    Napi::Value Consume(const Napi::CallbackInfo& info); 

    // Produce a value
    Napi::Value Produce(const Napi::CallbackInfo& info);

private:
    int Release();

    uint32_t num_elements;
    uint32_t element_size;
    uint32_t num_consumers;
    uint32_t consumer;

    size_t shm_size;
    void* shm_buf;
    
    sequence_t *consumers; // for each consumer, slot it's waiting to be filled
    sequence_t *cursor;    // next slot to be filled
    sequence_t *next;      // first free slot
    uint8_t* elements;
};

struct CloseFD
{
    void operator()(int *fd)
    {
        close(*fd);
        delete fd;
    }
};

void ThrowErrnoError(const Napi::CallbackInfo& info, const char *msg)
{
    int errnum = errno;
    char buf[1024] = {0};
    auto errmsg = strerror_r(errnum, buf, sizeof(buf));
    static_assert(std::is_same<decltype(errmsg), char*>::value,
                  "strerror_r must return char*");
    throw Napi::Error::New(info.Env(), 
        std::string(msg) + ": " + (errmsg ? errmsg : std::to_string(errnum)));
}

Disruptor::Disruptor(const Napi::CallbackInfo& info) :
    Napi::ObjectWrap<Disruptor>(info),
    shm_buf(MAP_FAILED)
{
    // Arguments
    Napi::String shm_name = info[0].As<Napi::String>();
    num_elements = info[1].As<Napi::Number>();
    element_size = info[2].As<Napi::Number>();
    num_consumers = info[3].As<Napi::Number>();
    bool init = info[4].As<Napi::Boolean>();
    consumer = info[5].As<Napi::Number>();

    // Open shared memory object
    std::unique_ptr<int, CloseFD> shm_fd(new int(
        shm_open(shm_name.Utf8Value().c_str(),
                 O_CREAT | O_RDWR | (init ? O_TRUNC : 0),
                 S_IRUSR | S_IWUSR)));
    if (*shm_fd < 0)
    {
        ThrowErrnoError(info, "Failed to open shared memory object");
    }

    // Allow space for all the elements,
    // a sequence number for each consumer,
    // the cursor sequence number (last filled slot) and
    // the next sequence number (first free slot).
    shm_size = (num_consumers + 2) * sizeof(sequence_t) +
               num_elements * element_size;

    // Resize the shared memory if we're initializing it.
    // Note: ftruncate initializes to null bytes.
    if (init && (ftruncate(*shm_fd, shm_size) < 0))
    {
        ThrowErrnoError(info, "Failed to size shared memory");
    }

    // Map the shared memory
    shm_buf = mmap(NULL,
                   shm_size,
                   PROT_READ | PROT_WRITE, MAP_SHARED,
                   *shm_fd,
                   0);
    if (shm_buf == MAP_FAILED)
    {
        ThrowErrnoError(info, "Failed to map shared memory");
    }

    consumers = static_cast<sequence_t*>(shm_buf);
    cursor = &consumers[num_consumers];
    next = &cursor[1];
    elements = reinterpret_cast<uint8_t*>(&next[1]);
}

Disruptor::~Disruptor()
{
    Release();
}

int Disruptor::Release()
{
    if (shm_buf != MAP_FAILED)
    {
        int r = munmap(shm_buf, shm_size);

        if (r < 0)
        {
            return r;
        }

        shm_buf = MAP_FAILED;
    }

    return 0;
}

Napi::Value Disruptor::Release(const Napi::CallbackInfo& info)
{
    if (Release() < 0)
    {
        ThrowErrnoError(info, "Failed to unmap shared memory");
    }

    return info.Env().Undefined();
}

Napi::Value Disruptor::Consume(const Napi::CallbackInfo& info)
{
    // Return all elements [&consumers[consumer], cursor)

    // TODO: Do we need to access consumer atomically if only we know only
    // this thread is updating it?
    sequence_t *ptr_consumer = &consumers[consumer];
    sequence_t seq_consumer = __sync_val_compare_and_swap(ptr_consumer, 0, 0);
    sequence_t seq_cursor = __sync_val_compare_and_swap(cursor, 0, 0);

    Napi::Array r = Napi::Array::New(info.Env());

    if (seq_cursor > seq_consumer)
    {
        r.Set(0U, Napi::Buffer<uint8_t>::New(
            info.Env(),
            elements + seq_consumer * element_size,
            (seq_cursor - seq_consumer) * element_size));
    }
    else if (seq_cursor < seq_consumer)
    {
        r.Set(0U, Napi::Buffer<uint8_t>::New(
            info.Env(),
            elements + seq_consumer * element_size,
            (num_elements - seq_consumer) * element_size));

        if (seq_cursor > 0)
        {
            r.Set(1U, Napi::Buffer<uint8_t>::New(
                info.Env(),
                elements,
                seq_cursor * element_size));
        }
    }

    // Options to return or busy wait?
    // Actually we need to callback - to prevent a copy of the data,
    // only update consumer sequence once callback returns
    // Does that mean we can't do async since the call will be done on
    // different (main) thread? Yes
    // We could pass back seq_cursor so app can call in with it to update

    // So either we do it synchronously or async with something to call back
    // into us to update consumer sequence.

    // Or allow both? Consume and ConsumeSync

        __sync_val_compare_and_swap(ptr_consumer, seq_consumer, seq_cursor);

    return r;
}

Napi::Value Disruptor::Produce(const Napi::CallbackInfo& info)
{
    // Check there is space (there isn't a consumer at next)

    // Claim slot, write to it and then commit, waiting for the cursor

    return info.Env().Undefined();
}

void Disruptor::Initialize(Napi::Env env, Napi::Object exports)
{
    exports["Disruptor"] = DefineClass(env, "Disruptor",
    {
        InstanceMethod("release", &Disruptor::Release),
        InstanceMethod("consume", &Disruptor::Consume),
        InstanceMethod("produce", &Disruptor::Produce)
    });
}

void Initialize(Napi::Env env, Napi::Object exports, Napi::Object module)
{
    Disruptor::Initialize(env, exports);
}

NODE_API_MODULE(disruptor, Initialize)