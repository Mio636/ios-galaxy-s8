/*
 * HakiosMali IOKit service and panfrost-compatible user client for the
 * Galaxy S8 (Mali-G71). Adapted from /home/mio/s9-ref/core/drivers/gpu/
 * hakios.mali.cpp, with the S9/IOSurface-specific selectors removed: this
 * bring-up client implements the Linux panfrost DRM requests Mesa needs
 * (create/mmap/get-offset, submit, wait, get-param, close) and no display
 * integration. The GPU is brought up only by hakios.mali.c with boot-arg
 * mali=9; this service refuses clients until that has happened.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/clock.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern "C" {
bool hakios_mali_ready(void);
bool hakios_mali_parameter(uint32_t which, uint64_t *value);
uint64_t hakios_mali_address(uint64_t size, bool executable);
bool hakios_mali_map(uint64_t gpu, uint64_t phys);
void hakios_mali_mapped(void);
void hakios_mali_unmap(uint64_t gpu, uint64_t size);
void hakios_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height, uint32_t row_bytes,
    uint32_t format, uint32_t offset);
uint64_t hakios_mali_enqueue(uint64_t chain, bool fragment);
bool hakios_mali_wait(uint64_t seq, uint64_t timeout_ns);
void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
void HakiosMaliStart(void);   /* called by bsd/dev/hakios.mali.c: C linkage matters */

uint32_t gMaliClients, gMaliBuffers, gMaliSubmits, gMaliFailed, gMaliShown;
struct mali_made { uint64_t address, size; uint32_t asked, flags; };
struct mali_made gMaliMade[32];
}

/* Linux's include/uapi/drm/panfrost_drm.h and drm.h request numbers. */
enum {
	kDrmGemClose            = 0x09,
	kPanfrostSubmit         = 0x40,
	kPanfrostWaitBO         = 0x41,
	kPanfrostCreateBO       = 0x42,
	kPanfrostMmapBO         = 0x43,
	kPanfrostGetParam       = 0x44,
	kPanfrostGetBOOffset    = 0x45,
	kPanfrostMadvise        = 0x48,
	kHakiosMaliWait         = 0x7d,     /* ours: wait for a job by sequence number */
};
#define PANFROST_JD_REQ_FS      (1u << 0)
#define PANFROST_BO_NOEXEC      (1u << 0)
#define PANFROST_BO_HEAP        (1u << 1)

struct drm_gem_close { uint32_t handle, pad; };
struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
struct drm_panfrost_wait_bo { uint32_t handle, pad; int64_t timeout_ns; };
struct drm_panfrost_create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct drm_panfrost_mmap_bo { uint32_t handle, flags; uint64_t offset; };
struct drm_panfrost_get_param { uint32_t param, pad; uint64_t value; };
struct drm_panfrost_get_bo_offset { uint32_t handle, pad; uint64_t offset; };
struct drm_panfrost_madvise { uint32_t handle, madv, retained; };
struct mali_wait { uint64_t seq; int64_t timeout_ns; };

#define MALI_BUFFERS            4096
#define MALI_BUFFER_MAX         (256ull << 20)

struct mali_buffer {
	IOMemoryDescriptor *memory;
	uint64_t address, size;
	uint64_t last_seq;
};

static IOLock *gMaliLock;

class HakiosMali : public IOService
{
	OSDeclareDefaultStructors(HakiosMali);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMali, IOService);

bool
HakiosMali::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "HakiosMaliUserClient");
	setName("HakiosMali");
	registerService();
	return true;
}

void
HakiosMaliStart(void)
{
	IOService *provider = IOService::getPlatform();
	HakiosMali *gpu;

	gMaliLock = IOLockAlloc();
	gpu = OSTypeAlloc(HakiosMali);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("HakiosMali: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("HakiosMali: the service did not start\n");
		gpu->release();
		return;
	}
	gpu->release();
	IOLog("HakiosMali: the GPU is there for programs (Mali-G71)\n");
}

class HakiosMaliUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(HakiosMaliUserClient);

	struct mali_buffer *fBuffers;
	uint32_t fNextBuffer;
	uint32_t fSubmits, fFailed, fRequests;

	struct mali_buffer *buffer(uint32_t handle);
	void freeBuffer(struct mali_buffer *b);
	IOReturn createBO(struct drm_panfrost_create_bo *request);
	struct mali_buffer *newBuffer(IOMemoryDescriptor *memory, uint64_t from, uint64_t size, uint32_t *handle,
	    uint32_t flags);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
	    APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(HakiosMaliUserClient, IOUserClient);

bool
HakiosMaliUserClient::start(IOService *provider)
{
	if (gMaliLock == NULL || !hakios_mali_ready() || !IOUserClient::start(provider)) {
		IOLog("HakiosMali: a client could not be started\n");
		return false;
	}
	OSIncrementAtomic((SInt32 *)&gMaliClients);
	fBuffers = IONewZero(struct mali_buffer, MALI_BUFFERS);
	return fBuffers != NULL;
}

struct mali_buffer *
HakiosMaliUserClient::buffer(uint32_t handle)
{
	if (handle == 0 || handle > MALI_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

void
HakiosMaliUserClient::freeBuffer(struct mali_buffer *b)
{
	if (b->last_seq != 0) {
		(void)hakios_mali_wait(b->last_seq, 2ull * NSEC_PER_SEC);
	}
	hakios_mali_unmap(b->address, b->size);
	b->memory->release();
	bzero(b, sizeof(*b));
}

struct mali_buffer *
HakiosMaliUserClient::newBuffer(IOMemoryDescriptor *memory, uint64_t from, uint64_t size, uint32_t *handle,
    uint32_t flags)
{
	struct mali_buffer *b = NULL;
	bool mapped = true;

	for (uint32_t n = 0; n < MALI_BUFFERS; n++) {
		uint32_t i = (fNextBuffer + n) % MALI_BUFFERS;

		if (fBuffers[i].memory == NULL) {
			b = &fBuffers[i];
			*handle = i + 1;
			fNextBuffer = i + 1;
			break;
		}
	}
	if (b == NULL) {
		return NULL;
	}
	b->address = hakios_mali_address(size, !(flags & PANFROST_BO_NOEXEC));
	for (uint64_t offset = 0; offset < size && mapped && b->address != 0; offset += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + offset, &length, kIOMemoryMapperNone);

		mapped = phys != 0 && hakios_mali_map(b->address + offset, phys);
	}
	if (b->address == 0 || !mapped) {
		if (b->address != 0) {
			hakios_mali_unmap(b->address, size);
		}
		bzero(b, sizeof(*b));
		return NULL;
	}
	hakios_mali_mapped();
	if (gMaliBuffers < 32) {
		gMaliMade[gMaliBuffers].address = b->address;
		gMaliMade[gMaliBuffers].size = size;
		gMaliMade[gMaliBuffers].asked = (uint32_t)size;
		gMaliMade[gMaliBuffers].flags = flags;
	}
	OSIncrementAtomic((SInt32 *)&gMaliBuffers);
	b->size = size;
	return b;
}

IOReturn
HakiosMaliUserClient::createBO(struct drm_panfrost_create_bo *request)
{
	uint64_t size = round_page_64(request->size);
	IOBufferMemoryDescriptor *memory;
	struct mali_buffer *b;

	if (size == 0 || size > MALI_BUFFER_MAX) {
		return kIOReturnBadArgument;
	}
	memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMapWriteCombineCache,
	    (vm_size_t)size, PAGE_SIZE);
	if (memory == NULL) {
		return kIOReturnNoMemory;
	}
	b = newBuffer(memory, 0, size, &request->handle, request->flags);
	if (b == NULL) {
		memory->release();
		return kIOReturnNoMemory;
	}
	b->memory = memory;
	request->offset = b->address;
	hakios_mali_tag(b->address, 1, 0, 0, 0, 0, 0);
	return kIOReturnSuccess;
}

IOReturn
HakiosMaliUserClient::clientClose(void)
{
	IOLockLock(gMaliLock);
	for (uint32_t i = 0; fBuffers != NULL && i < MALI_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	IOLockUnlock(gMaliLock);
	if (fSubmits != 0) {
		IOLog("HakiosMali: a client goes after %u job chains, %u of them unfinished\n", fSubmits, fFailed);
	}
	terminate();
	return kIOReturnSuccess;
}

void
HakiosMaliUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct mali_buffer, MALI_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
HakiosMaliUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory)
{
	struct mali_buffer *b;
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gMaliLock);
	b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gMaliLock);
	return ret;
}

IOReturn
HakiosMaliUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panfrost_submit submit;
		struct drm_panfrost_wait_bo wait;
		struct drm_panfrost_create_bo create;
		struct drm_panfrost_mmap_bo mmap;
		struct drm_panfrost_get_param param;
		struct drm_panfrost_get_bo_offset offset;
		struct drm_panfrost_madvise madvise;
		struct mali_wait waitseq;
	} r;
	size_t size = args->structureInputSize;
	struct mali_buffer *b;
	IOReturn ret = kIOReturnSuccess;

	(void)dispatch;
	(void)target;
	(void)reference;
	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!hakios_mali_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	if (selector == kHakiosMaliWait || selector == kPanfrostWaitBO) {
		uint64_t seq = r.waitseq.seq;
		int64_t timeout = r.waitseq.timeout_ns;

		if (selector == kPanfrostWaitBO) {
			IOLockLock(gMaliLock);
			b = buffer(r.wait.handle);
			seq = b != NULL ? b->last_seq : 0;
			IOLockUnlock(gMaliLock);
			timeout = r.wait.timeout_ns;
			if (b == NULL) {
				return kIOReturnNotFound;
			}
		}
		ret = seq == 0 || hakios_mali_wait(seq, timeout > 0 ? (uint64_t)timeout : 0) ?
		    kIOReturnSuccess : kIOReturnTimeout;
		if (args->structureOutput != NULL && args->structureOutputSize >= size) {
			memcpy(args->structureOutput, &r, size);
			args->structureOutputSize = (uint32_t)size;
		}
		return ret;
	}

	IOLockLock(gMaliLock);
	if (fRequests++ < 32) {
		IOLog("HakiosMali: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanfrostGetParam:
		ret = hakios_mali_parameter(r.param.param, &r.param.value) ? kIOReturnSuccess : kIOReturnBadArgument;
		break;
	case kPanfrostCreateBO:
		ret = createBO(&r.create);
		break;
	case kPanfrostMmapBO:
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = (uint64_t)r.mmap.handle << 12;
		break;
	case kPanfrostGetBOOffset:
		b = buffer(r.offset.handle);
		ret = b != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.offset.offset = b != NULL ? b->address : 0;
		break;
	case kPanfrostSubmit: {
		uint64_t seq = 0;

		fSubmits++;
		OSIncrementAtomic((SInt32 *)&gMaliSubmits);
		if (r.submit.jc != 0) {
			seq = hakios_mali_enqueue(r.submit.jc, r.submit.requirements & PANFROST_JD_REQ_FS);
		}
		for (uint32_t i = 0; seq != 0 && i < r.submit.bo_handle_count && i < 4096; i += 64) {
			uint32_t handles[64];
			uint32_t n = r.submit.bo_handle_count - i < 64 ? r.submit.bo_handle_count - i : 64;

			if (copyin((user_addr_t)(r.submit.bo_handles + i * sizeof(uint32_t)), handles,
			    n * sizeof(uint32_t)) != 0) {
				break;
			}
			for (uint32_t k = 0; k < n; k++) {
				b = buffer(handles[k]);
				if (b != NULL) {
					b->last_seq = seq;
				}
			}
		}
		r.submit.jc = seq;
		break;
	}
	case kPanfrostMadvise:
		r.madvise.retained = 1;
		break;
	case kDrmGemClose:
		b = buffer(r.close.handle);
		if (b != NULL) {
			freeBuffer(b);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	default:
		IOLog("HakiosMali: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gMaliLock);

	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t)size;
	}
	return ret;
}
