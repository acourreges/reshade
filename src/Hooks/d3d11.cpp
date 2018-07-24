#include "Log.hpp"
#include "HookManager.hpp"
#include "Runtimes\RuntimeD3D11.hpp"

#include <d3d11.h>
#include <set>
#include <mutex>
#include <boost/unordered_set.hpp>
#include <boost/filesystem.hpp>

// Set these to your game resolution
// Simplify tracking since RTs are created prior to redimensioning the swap-chain to the final resolution
#define BACK_BUFFER_WIDTH 1280
#define BACK_BUFFER_HEIGHT 720

// How many 720p scratch textures to use when snapshotting RTs in the middle of a command list execution
#define POOL_720P_COUNT 200

// Dump vertex shaders, pixel shaders...
bool DumpShaderVS = false;
bool DumpShaderGS = false;
bool DumpShaderPS = false;
bool DumpShaderCS = false;

// -----------------------------------------------------------------------------------------------------

extern const GUID sRuntimeGUID;

extern boost::filesystem::path GameFolderPath;

extern int BackbufferWidth;
extern int BackbufferHeight;

extern float BandageCurrent;
// -----------------------------------------------------------------------------------------------------



std::vector<ID3D11Texture2D*> Pool_RGB8_Typeless_720p;

std::mutex deferredDrawMutex;

HRESULT STDMETHODCALLTYPE ID3D11DeviceContext_FinishCommandList(ID3D11DeviceContext *pDeviceContext, BOOL RestoreDeferredContextState,
	ID3D11CommandList **ppCommandList);

ULONG STDMETHODCALLTYPE ID3D11RenderTargetView_Release(ID3D11RenderTargetView *rt);

ULONG STDMETHODCALLTYPE ID3D11CommandList_Release(ID3D11CommandList *cl)
{
	// /!\___ NOT USED ____/!\

	static const auto trampoline = ReShade::Hooks::Call(&ID3D11CommandList_Release);

	const ULONG ref = trampoline(cl);

	return ref;
}

static std::set<ID3D11RenderTargetView*> ViewsUsedAsRTs;

static std::set<void*> AllRTs;
static std::mutex AllRTsMutex;

std::set<void*> AllCLs;
std::set<void*> RecentCLErrors;
static std::mutex AllCLsMutex;

//TODO make local to device
static ID3D11DeviceContext* AllDeferredContexts[20]; //Usually 8 in MGSV
static UINT AllDeferredContextsCount = 0;

static bool IsContextDeferred(ID3D11DeviceContext* ctx) {
	for (int i = 0; i < AllDeferredContextsCount; i++) {
		if (AllDeferredContexts[i] == ctx) return true;
	}
	return false;
}

class DefEvent {
public:
	std::string label;

};

// MetaCL
class MetaCL {
public:
	ID3D11DeviceContext* originalContext;
	std::vector<DefEvent> events;

	std::vector<ID3D11CommandList*> cls;

	std::set<ID3D11RenderTargetView*> RTsTouched; // Any RT touched by this draw call
	std::vector<ID3D11RenderTargetView*> copiedViews;


	MetaCL() {}

	void OnDraw(UINT count) {

		if (!ReShade::Runtimes::D3D11Runtime::mIsDumpingTrace) return;

		LOG(INFO) << "OnDraw count: " << count << " on context " << originalContext << " total records: " << events.size();
		DefEvent evt;
		evt.label = "Draw " + std::to_string(count);
		events.push_back(evt);

		int c = events.size();
		// Regularly snapshot RT contents into our scratch buffers (every time N draw calls have been processed by the CL)
		// Can snapshot more often if memory allows to have more sratch textures
		if (c == 1 || c == 20 || c == 50 || c == 100 || c == 200 || c == 400) {
			//OnSplitCL(); // Seems to cause crash
			SnapshotRTs();
		}
	}

	void SnapshotRTs() {
		if (!ReShade::Runtimes::D3D11Runtime::mIsDumpingTrace) return;
		// Command to populate scratch textures. Will be read and dumped to HDD at the end
		// of the CL replay.

		//LOG(INFO) << "About to snapshot " << this << " Watched: " << ReShade::Runtimes::D3D11Runtime::WatchedRTs.size()
		//	<< " Seen: " << RTsTouched.size();

		//for (auto rt : ViewsUsedAsRTs) {
			for (auto watchedRT : ReShade::Runtimes::D3D11Runtime::WatchedRTs) {
					auto rt = watchedRT;
					if (copiedViews.size() == Pool_RGB8_Typeless_720p.size()) {
						LOG(ERROR) << "Exhausted all the RT sctratch pool for this CL";
						continue;
					}
					ID3D11Texture2D* scratch = Pool_RGB8_Typeless_720p.at(copiedViews.size());
					//Copy it
					LOG(INFO) << "Copying to scratch " << rt;

					ID3D11Resource* resource;
					rt->GetResource(&resource);

					ID3D11Texture2D *texture = nullptr;
					HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));

					if (FAILED(hr))
					{
						LOG(INFO) << "Was not a texture 2D!";
						continue;
					}

					D3D11_TEXTURE2D_DESC desc;
					texture->GetDesc(&desc);

					// We could dump all the tracked RTs but that's a lot, limit to the most interesting formats and resolution
					if (desc.Width != BACK_BUFFER_WIDTH || desc.Height != BACK_BUFFER_HEIGHT) continue;

					if (
						desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS
						//&& desc.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS
						)
					{
						continue;
					}

					originalContext->CopyResource(scratch, texture);
					copiedViews.push_back(rt);

					LOG(INFO) << "Scratch copied " << texture << "(RT " << rt << " into " << scratch << ")";

					texture->Release();
			}
		//}
	}

	void DumpToDisk(boost::filesystem::path rootPathNoExt, ReShade::Runtimes::D3D11Runtime* runtime) { // path

		if (events.size() == 0 && copiedViews.size() == 0) return;

		LOG(INFO) << "Dumping CL information in " << rootPathNoExt;

		//Write event log
		boost::filesystem::path logPath = rootPathNoExt / "BucketLog.txt";

		//Make sure parent folder exists
		boost::system::error_code returnedError;
		boost::filesystem::create_directories(logPath.parent_path(), returnedError);

		std::ofstream outfile(logPath.string());
		if (!outfile.is_open()) {
			LOG(ERROR)<< "Couldn't open text file to write" << logPath.string();
		} else {
			LOG(INFO) << "Writing event log: " << logPath.string();
			outfile << "(Events: " << events.size() << " RT Snapshots: " << copiedViews.size() << ")" << std::endl;
			for (auto event : events) {
				outfile << event.label << std::endl;
			}
			outfile.close();
		}

		//Write images
		char tempChar[260];
		for (int i = 0; i < copiedViews.size(); i++) {
			ID3D11RenderTargetView* rt = copiedViews.at(i);
			snprintf(tempChar, 260, "RT_%p_%04d", rt, i);
			boost::filesystem::path imgPath = rootPathNoExt / std::string(tempChar);

			runtime->DumpReadableTexture2D(imgPath, Pool_RGB8_Typeless_720p.at(i));
		}
	}

	void onSetRT(ID3D11RenderTargetView* rt)
	{
		if (rt) {
			// RTsTouched.insert(rt);
			ViewsUsedAsRTs.insert(rt);
			// TODO add "Using RT" event in bucket log
		}
	}

	void OnSplitCL()
	{
		ID3D11CommandList* cl;
		//LOG(INFO) << "About to split: " << this << " from context :" << originalContext;

		static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_FinishCommandList);
		const HRESULT hr = trampoline(originalContext, FALSE, &cl);

		//LOG(INFO) << "Trampoline result address " << cl;
		if (!SUCCEEDED(hr)) {
			LOG(ERROR) << "Trampoline to CL finish failed. HR " << hr;
		}

		////HRESULT hr = originalContext->FinishCommandList(FALSE, &cl);

		if (cl) cls.push_back(cl);
		//LOG(INFO) << "Splitted CL: " << cl << " Now has a total count of " << cls.size();
	}

	~MetaCL() 
	{
		//LOG(INFO) << "Destroy MetaCL " << this << " with splitted count " << cls.size();
		for (auto cl : cls) 
		{	
			//LOG(INFO) << "Releasing splitted CL: " << cl;
			static const auto trampoline = ReShade::Hooks::Call(&ID3D11RenderTargetView_Release);
			const ULONG ref = trampoline((ID3D11RenderTargetView*)cl);
			//LOG(INFO) << "Splitted CL release, new ref count " << ref;
		}
		
	}
};

//Hashmap client CL pointer -> MetaCL (will have to be released and deleted )
static std::unordered_map< void*, MetaCL* > AllMetaCLs;
static std::mutex AllMetaCLsMutex;

static void AllMetaCLs_SafeAdd(void* cl, MetaCL* metaCL)
{
	AllMetaCLsMutex.lock();
	// Check it's not already present
	if (AllMetaCLs.find(cl) != AllMetaCLs.end()) 
	{
		LOG(ERROR) << "Saving a new meta CL which already exists " << cl;
	} 
	else 
	{
		AllMetaCLs[cl] = metaCL;
	}
	AllMetaCLsMutex.unlock();
}

static void AllMetaCLs_SafeRemove(void* cl)
{
	AllMetaCLsMutex.lock();
	// Check it's not already present
	auto item = AllMetaCLs.find(cl);
	if (item == AllMetaCLs.end()) 
	{
		LOG(ERROR) << "Removing a meta CL which was not here " << cl;
	} 
	else 
	{
		MetaCL* metaCL = item->second;
		delete metaCL;
		AllMetaCLs.erase(cl);
	}
	AllMetaCLsMutex.unlock();
}

static MetaCL* AllMetaCLs_SafeGet(void* cl)
{
	AllMetaCLsMutex.lock();
	// Check it's not already present
	auto item = AllMetaCLs.find(cl);
	if (item == AllMetaCLs.end()) 
	{
		AllMetaCLsMutex.unlock();
		return nullptr;
	}
	else 
	{
		MetaCL* metaCL = item->second;
		AllMetaCLsMutex.unlock();
		return metaCL;
	}
}

// Deferred context recording a command list
class DefContext
{
public:
	ID3D11DeviceContext* ctx;

	MetaCL* metaCL;

	DefContext(ID3D11DeviceContext* c) : ctx(c), metaCL(nullptr) {}

	void BeginRecording() {
		if (metaCL) LOG(ERROR) << "Recording a new command list but some metaCL is already present!";
		metaCL = new MetaCL;
		metaCL->originalContext = ctx;
	}

	MetaCL* EndRecording() 
	{
		//LOG(INFO) << "Ending recording metaCL " << this << " Context is " << ctx;

		metaCL->OnSplitCL();
		if (metaCL->events.size() > 0) metaCL->SnapshotRTs();

		//Prepare metaCL
		MetaCL* result = metaCL;
		//Add it to the currently alive CLs
		//AllMetaCLs_SafeAdd(publicCL, metaCL);
		metaCL = nullptr;

		BeginRecording();
		return result;
	}
};


//Hashmap context pointer -> DefContext pointer
std::unordered_map< ID3D11DeviceContext*, DefContext* > AllDefContexts;
static DefContext* AllDefContexts_Get(ID3D11DeviceContext* ctx)
{
	//AllMetaCLsMutex.lock();
	// Check it's not already present
	auto item = AllDefContexts.find(ctx);
	if (item == AllDefContexts.end()) {
		//AllMetaCLsMutex.unlock();
		return nullptr;
	}
	else {
		DefContext* ct = item->second;
		//AllMetaCLsMutex.unlock();
		return ct;
	}
}


static DefContext* GetDefContextIfExists(ID3D11DeviceContext* ctx)
{
	bool isDeferred = IsContextDeferred(ctx);
	if (!isDeferred) return nullptr;
	auto it = AllDefContexts.find(ctx);
	if (it == AllDefContexts.end()) 
	{
		LOG(ERROR) << "Draw call on a context we are not tracking! Ctx: " << ctx;
		return nullptr;
	}
	DefContext* defCtx = it->second;
	return defCtx;
}

static void OnDeferredDraw(ID3D11DeviceContext* ctx, UINT vertCount)
{
	deferredDrawMutex.lock();

	// Find the defContext
	// TODO lock this. Might still work since the usual pattern is only concurrent reads. Ok for now
	auto it = AllDefContexts.find(ctx);
	if (it == AllDefContexts.end()) 
	{
		LOG(ERROR) << "Draw call on a context we are not tracking! Ctx: " << ctx;
		return;
	}
	DefContext* defCtx = it->second;

	// Don't need locking here, usually one thread reserved to one deferred context
	if (!defCtx->metaCL) 
	{
		LOG(ERROR) << "This deferred context was not recording! Ctx: " << ctx;
		return;
	}

	defCtx->metaCL->OnDraw(vertCount); // todo pass context so we can finish too
	deferredDrawMutex.unlock();
}

// ID3D11DepthStencilView
ULONG STDMETHODCALLTYPE ID3D11DepthStencilView_Release(ID3D11DepthStencilView *pDepthStencilView)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DepthStencilView_Release);

	ID3D11Device *device = nullptr;
	pDepthStencilView->GetDevice(&device);

	assert(device != nullptr);

	const ULONG ref = trampoline(pDepthStencilView);

	if (ref == 0)
	{
		ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
		UINT size = sizeof(runtime);

		if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
		{
			runtime->OnDeleteDepthStencilView(pDepthStencilView);
		}
	}

	device->Release();

	return ref;
}

ULONG STDMETHODCALLTYPE ID3D11RenderTargetView_Release(ID3D11RenderTargetView *rt)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11RenderTargetView_Release);

	ID3D11Device *device = nullptr;
	rt->GetDevice(&device);

	assert(device != nullptr);

	ULONG ref;
	ref = trampoline(rt);

	//LOG(INFO) << "RT freed " << rt << " refCount " << ref;

	if (ref == 0)
	{
		ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
		UINT size = sizeof(runtime);

		if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
		{
			//runtime->RemoveWatchRT(rt);
		}
		
		// Checks if it was a RT
		{
			AllRTsMutex.lock();
			int erasedCount = AllRTs.erase(rt);
			if (erasedCount > 0) {
				LOG(INFO) << "RT freed " << rt;
				runtime->RemoveWatchRT(rt);
			}
			AllRTsMutex.unlock();
		}

		// Checks if it was a CL
		{
			AllCLsMutex.lock();

			// Remove it, unless it's a recent error...
			int erasedCount = RecentCLErrors.erase(rt);

			if (erasedCount > 0) {
				LOG(INFO) << "Warning: recovered from sync issue " << rt;
			}
			else {
				// Normal case, delete
				int erasedCount = AllCLs.erase(rt);
				if (erasedCount > 0) {
					//LOG(INFO) << "CL freed " << rt;
					AllMetaCLs_SafeRemove(rt);
				}
			}
			AllCLsMutex.unlock();
		}
		
	}

	device->Release();
	return ref;
}

static int executedCLCount = 0;

void STDMETHODCALLTYPE ID3D11DeviceContext_ExecuteCommandList(ID3D11DeviceContext *pDeviceContext, ID3D11CommandList *pCommandList,
BOOL RestoreContextState)
{

	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_ExecuteCommandList);

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	//LOG(INFO) << "Execute CL. Restore: " << RestoreContextState  << " Thread: " << GetCurrentThreadId() << " Pointer: " << pCommandList << " context: " << pDeviceContext;

	ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
	ID3D11DepthStencilView* depthstencil = nullptr;

	// This always return 0 MRT bound. All draw calls are in actually in deferred contexts...
	pDeviceContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthstencil);

	int boundCount = 0;
	for (auto r : targets) {
		if (r) boundCount++;
	}
	//LOG(INFO) << "Currently bound: " << boundCount;

	AllCLsMutex.lock();
	auto result = AllCLs.find(pCommandList);
	if (result == AllCLs.end()) LOG(ERROR) << "Executing a CL we were not tracking! " << pCommandList;
	AllCLsMutex.unlock();

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		//runtime->OnExecuteCommandList(pDeviceContext, pCommandList, RestoreContextState);
		if (runtime->mIsDumpingTrace) executedCLCount++;
		else executedCLCount = 0;
	}

	device->Release();

	// Do we have any meta data for this CL?
	MetaCL* metaCL = AllMetaCLs_SafeGet(pCommandList);
	if (metaCL) {
		int clsCount = metaCL->cls.size();
		//LOG(INFO) << "Will run metadata for CL " << pCommandList << " CLs: " << (clsCount);

		// Run it!
		if (runtime) {
			for (int i = 0; i <clsCount; i++) {
				//runtime->mImmediateContext->ExecuteCommandList(metaCL->cls.at(i), false);
				trampoline(pDeviceContext, metaCL->cls.at(i), false);
			}

			if (runtime->mIsDumpingTrace) {
				char tmp[260];
				snprintf(tmp, 260, "%s/%03d_Bucket", runtime->DumpRootPath.string().c_str(), executedCLCount);
				boost::filesystem::path dumpRoot = tmp;
				LOG(INFO) << "Dumping metaCL " << metaCL << " into " << dumpRoot.string();
				metaCL->DumpToDisk(dumpRoot, runtime);
			}
		}
		else {
			LOG(ERROR) << "No runtime available to replay CL! " << pCommandList;
		}

	} else {
		LOG(INFO) << "Found NO metadata for CL " << pCommandList;
	}
	//TODO execute partial CLs stored in MetaCL

	trampoline(pDeviceContext, pCommandList, false);
}



void STDMETHODCALLTYPE ID3D11DeviceDeferredContext_DrawIndexed(ID3D11DeviceContext *pDeviceContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceDeferredContext_DrawIndexed);

	//LOG(INFO) << "[DEFERRED] DrawIndexed. Vertices: " << IndexCount << " Thread: " << GetCurrentThreadId();

	trampoline(pDeviceContext, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexed(ID3D11DeviceContext *pDeviceContext, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_DrawIndexed);
	bool IsDeffered = IsContextDeferred(pDeviceContext);
	
	// These are the draw calls corresponding to Ishmael's bandage, just mess with the total triangle count
	if (IndexCount == 1425 && StartIndexLocation == 61449) {
		IndexCount = BandageCurrent * IndexCount;
		if (IndexCount == 0) return;
	} else if (IndexCount == 2895 && StartIndexLocation == 64089) {
		IndexCount = BandageCurrent * IndexCount;
		if (IndexCount == 0) return;
	}
	
	//LOG(INFO) << "DrawIndexed." << (IsDeffered? "DEF" : "IM") << " Vertices: " << IndexCount << IndexCount << " Thread: " << GetCurrentThreadId() << " device context: " << pDeviceContext;

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		runtime->OnDrawInternal(pDeviceContext, IndexCount);
	}

	device->Release();

	trampoline(pDeviceContext, IndexCount, StartIndexLocation, BaseVertexLocation);

	if (IsDeffered) {
		OnDeferredDraw(pDeviceContext, IndexCount);
	}
	
}
void STDMETHODCALLTYPE ID3D11DeviceContext_Draw(ID3D11DeviceContext *pDeviceContext, UINT VertexCount, UINT StartVertexLocation)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_Draw);

	bool IsDeffered = IsContextDeferred(pDeviceContext);

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		runtime->OnDrawInternal(pDeviceContext, VertexCount);
	}

	device->Release();

	trampoline(pDeviceContext, VertexCount, StartVertexLocation);

	if (IsDeffered) {
		OnDeferredDraw(pDeviceContext, VertexCount);
	}
}
void STDMETHODCALLTYPE ID3D11DeviceContext_DrawIndexedInstanced(ID3D11DeviceContext *pDeviceContext, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_DrawIndexedInstanced);

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		runtime->OnDrawInternal(pDeviceContext, IndexCountPerInstance * InstanceCount);
	}

	device->Release();

	trampoline(pDeviceContext, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}
void STDMETHODCALLTYPE ID3D11DeviceContext_DrawInstanced(ID3D11DeviceContext *pDeviceContext, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_DrawInstanced);

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		runtime->OnDrawInternal(pDeviceContext, VertexCountPerInstance * InstanceCount);
	}

	device->Release();

	trampoline(pDeviceContext, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}
void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetRenderTargets(ID3D11DeviceContext *pDeviceContext, UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_OMSetRenderTargets);

	DefContext* defCtx = GetDefContextIfExists(pDeviceContext);

	//LOG(INFO) << "Set RTs. Count: " << NumViews << " DefCtx: " << defCtx << " ctx: " << pDeviceContext;

	for (int i = 0; i < NumViews; i++) {
		ID3D11RenderTargetView* rt = ppRenderTargetViews[i];
		if (!rt) continue;
		defCtx->metaCL->onSetRT(rt);
	}

	if (defCtx) {
		for (int i = 0; i < NumViews; i++) {
			defCtx->metaCL->onSetRT(ppRenderTargetViews[i]);
		}
	}

	if (pDepthStencilView != nullptr)
	{
		ID3D11Device *device = nullptr;
		pDeviceContext->GetDevice(&device);

		assert(device != nullptr);

		ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
		UINT size = sizeof(runtime);

		if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
		{
			runtime->OnSetDepthStencilView(pDepthStencilView);
		}

		device->Release();
	}

	trampoline(pDeviceContext, NumViews, ppRenderTargetViews, pDepthStencilView);
}
void STDMETHODCALLTYPE ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext *pDeviceContext, UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts)
{
	//LOG(INFO) << "Set UAV";

	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews);

	DefContext* defCtx = GetDefContextIfExists(pDeviceContext);

	if (defCtx) {
		for (int i = 0; i < NumRTVs; i++) {
			defCtx->metaCL->onSetRT(ppRenderTargetViews[i]);
		}
	}

	if (pDepthStencilView != nullptr)
	{
		ID3D11Device *device = nullptr;
		pDeviceContext->GetDevice(&device);

		assert(device != nullptr);

		ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
		UINT size = sizeof(runtime);

		if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
		{
			runtime->OnSetDepthStencilView(pDepthStencilView);
		}

		device->Release();
	}

	trampoline(pDeviceContext, NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}
void STDMETHODCALLTYPE ID3D11DeviceContext_CopyResource(ID3D11DeviceContext *pDeviceContext, ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_CopyResource);

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		runtime->OnCopyResource(pDstResource, pSrcResource);
	}

	device->Release();

	trampoline(pDeviceContext, pDstResource, pSrcResource);
}
void STDMETHODCALLTYPE ID3D11DeviceContext_ClearDepthStencilView(ID3D11DeviceContext *pDeviceContext, ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_ClearDepthStencilView);

	ID3D11Device *device = nullptr;
	pDeviceContext->GetDevice(&device);

	assert(device != nullptr);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		runtime->OnClearDepthStencilView(pDepthStencilView);
	}

	device->Release();

	trampoline(pDeviceContext, pDepthStencilView, ClearFlags, Depth, Stencil);
}
void STDMETHODCALLTYPE ID3D11DeviceContext_OMGetRenderTargets(ID3D11DeviceContext *pDeviceContext, UINT NumViews, ID3D11RenderTargetView **ppRenderTargetViews, ID3D11DepthStencilView **ppDepthStencilView)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_OMGetRenderTargets);

	trampoline(pDeviceContext, NumViews, ppRenderTargetViews, ppDepthStencilView);

	if (ppDepthStencilView != nullptr)
	{
		ID3D11Device *device = nullptr;
		pDeviceContext->GetDevice(&device);

		assert(device != nullptr);

		ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
		UINT size = sizeof(runtime);

		if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
		{
			runtime->OnGetDepthStencilView(*ppDepthStencilView);
		}

		device->Release();
	}
}

void STDMETHODCALLTYPE ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews(ID3D11DeviceContext *pDeviceContext, UINT NumRTVs, ID3D11RenderTargetView **ppRenderTargetViews, ID3D11DepthStencilView **ppDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews);

	if (ppDepthStencilView != nullptr)
	{
		ID3D11Device *device = nullptr;
		pDeviceContext->GetDevice(&device);

		assert(device != nullptr);

		ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
		UINT size = sizeof(runtime);

		if (SUCCEEDED(device->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
		{
			runtime->OnGetDepthStencilView(*ppDepthStencilView);
		}

		device->Release();
	}

	trampoline(pDeviceContext, NumRTVs, ppRenderTargetViews, ppDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews);
}

// ID3D11Device
HRESULT STDMETHODCALLTYPE ID3D11Device_CreateDepthStencilView(ID3D11Device *pDevice, ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreateDepthStencilView);

	const HRESULT hr = trampoline(pDevice, pResource, pDesc, ppDepthStencilView);

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(hr) && SUCCEEDED(pDevice->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		ReShade::Hooks::Register(VTABLE(*ppDepthStencilView), 2, reinterpret_cast<ReShade::Hook::Function>(&ID3D11DepthStencilView_Release));

		runtime->OnCreateDepthStencilView(pResource, *ppDepthStencilView);
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateRenderTargetView(
	ID3D11Device *pDevice,
	ID3D11Resource *pResource,
	const D3D11_RENDER_TARGET_VIEW_DESC *pDesc,
	ID3D11RenderTargetView **ppRTView)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreateRenderTargetView);

	const HRESULT hr = trampoline(pDevice, pResource, pDesc, ppRTView);

	LOG(INFO) << "Create RT order from tread " << GetCurrentThreadId() << " Created is " << *ppRTView;
	
	//AllRTs

	ReShade::Runtimes::D3D11Runtime *runtime = nullptr;
	UINT size = sizeof(runtime);

	if (SUCCEEDED(hr) && SUCCEEDED(pDevice->GetPrivateData(sRuntimeGUID, &size, reinterpret_cast<void *>(&runtime))))
	{
		ReShade::Hooks::Register(VTABLE(*ppRTView)[2], reinterpret_cast<ReShade::Hook::Function>(&ID3D11RenderTargetView_Release));

		{
			AllRTsMutex.lock();
			auto result = AllRTs.insert(*ppRTView);
			if (!result.second) LOG(ERROR) << "Adding an RT to track which was already tracked! " << *ppRTView;
			AllRTsMutex.unlock();
		}

		ID3D11Texture2D *texture = nullptr;
		HRESULT hr2 = pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));

		if (SUCCEEDED(hr2))
		{
			D3D11_TEXTURE2D_DESC desc;
			texture->GetDesc(&desc);

			//LOG(INFO) << "Creating RT! Format: " << desc.Format << " Size " << desc.Width << "x" << desc.Height << " Address: " << *ppRTView;

			// Real backbuffer NOT known yet, MGS calls ResizeBuffers() later with the final resolution
			// Use hardcoded values to begin tracking immediately
			int gameWidth = BACK_BUFFER_WIDTH;
			int gameHeight = BACK_BUFFER_HEIGHT;

			bool acceptRT = (desc.Width == gameWidth && desc.Height == gameHeight &&
				(
					// base, albedo, normal, roughness, velocity(?), SSAO
					desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS /* || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB */
					// GI diffuse, specular, SSR (DoF too but needs half-res)
					|| desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS
					)
				);

			if (!acceptRT)
			{
				// Bloom at 1/8th resolution
				acceptRT = desc.Width == (gameWidth >> 3) && (desc.Height == gameHeight >> 3) &&
					desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;

			}

			if (!acceptRT)
			{
				// Bloom at 1/8th resolution
				acceptRT = desc.Width == (gameWidth >> 3) && (desc.Height == gameHeight >> 3) &&
					desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS;

			}

			if (!acceptRT)
			{
				// SSR
				acceptRT = desc.Width == (gameWidth >> 1) && (desc.Height == gameHeight >> 1) &&
					desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;

			}

			if (!acceptRT)
			{
				// SSAO
				acceptRT = desc.Width == (gameWidth >> 0) && (desc.Height == gameHeight >> 0) &&
					desc.Format == DXGI_FORMAT_R8_TYPELESS;

			}

			if (acceptRT) {
				runtime->AddWatchRT(*ppRTView);
			}

			texture->Release();
		}
	}
	else {
		LOG(INFO) << "No runtime - RT create";
	}
		return hr;

}


HRESULT STDMETHODCALLTYPE ID3D11Device_CheckFeatureSupport(
	ID3D11Device *pDevice,
	D3D11_FEATURE Feature,
	void *pFeatureSupportData,
	UINT FeatureSupportDataSize)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CheckFeatureSupport);

	const HRESULT hr = trampoline(pDevice, Feature, pFeatureSupportData, FeatureSupportDataSize);

	LOG(INFO) << "Asked Feature Support " << Feature << "Size " << FeatureSupportDataSize;

	if (Feature == D3D11_FEATURE_THREADING)
	{
		LOG(INFO) << "Overriding threading";
		memset(pFeatureSupportData, 0, FeatureSupportDataSize);
	}

	return hr;
}



HRESULT STDMETHODCALLTYPE ID3D11Device_CreateDeferredContext(
	ID3D11Device *pDevice,
	UINT ContextFlags,
	ID3D11DeviceContext **ppDeferredContext)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreateDeferredContext);

	
#if 1
	const HRESULT hr = trampoline(pDevice, ContextFlags, ppDeferredContext);

	//TODO sync this? Expecting deferred context to be created from a single thread... -> single threaded fine for now
	LOG(INFO) << "Create deferred context. Pointer: " << *ppDeferredContext << " So far seen: " << AllDeferredContextsCount << " On thread: " << GetCurrentThreadId();

	AllDeferredContexts[AllDeferredContextsCount] = *ppDeferredContext;
	AllDeferredContextsCount++;

	// Also add them to our tracking of CLs
	DefContext* defCtx = new DefContext(*ppDeferredContext);
	AllDefContexts[*ppDeferredContext] = defCtx;
	defCtx->BeginRecording();

	// TODO handle release/free, but app doesn't use that many contexts
	//ReShade::Hooks::Register(VTABLE(ppDeferredContext)[12], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceDeferredContext_DrawIndexed));

	return hr;
#else
	*ppDeferredContext = nullptr;
	return DXGI_ERROR_INVALID_CALL;
#endif
}

// Tracked shader types
enum DumpShaderType {
	VS,
	GS,
	PS,
	CS
};

static void DumpShader(DumpShaderType shaderType, const void* data, SIZE_T dataSize, void* apiPointer) {
	char* typeStr;
	switch (shaderType) {
		case VS: 
			if (!DumpShaderVS) return;
			typeStr = "VS"; 
			break;
		case GS: 
			if (!DumpShaderGS) return;
			typeStr = "GS"; 
			break;
		case PS: 
			if (!DumpShaderPS) return;
			typeStr = "PS"; 
			break;
		case CS: 
			if (!DumpShaderCS) return;
			typeStr = "CS"; 
			break;
		default: return;
	}

	if (shaderType != PS) return;

	//LOG(INFO) << "Dumping " << typeStr << " shader. Size: " << dataSize << " API Pointer: " << dataSize;

	char tempChar[260];
	snprintf(tempChar, 260, "%s_%p.shader", typeStr, apiPointer);

	boost::filesystem::path path = GameFolderPath.parent_path() / std::string("ShaderDump") / std::string(tempChar);

	boost::system::error_code returnedError;
	boost::filesystem::create_directories(path.parent_path(), returnedError);

	std::ofstream outfile(path.string(), std::ofstream::binary);
	outfile.write((char*)data, dataSize);
	outfile.close();
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateVertexShader(
	ID3D11Device *pDevice,
	const void *pShaderBytecode,
	SIZE_T BytecodeLength,
	ID3D11ClassLinkage *pClassLinkage,
	ID3D11VertexShader **ppVertexShader)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreateVertexShader);
	const HRESULT hr = trampoline(pDevice, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
	DumpShader(VS, pShaderBytecode, BytecodeLength, ppVertexShader);
	return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateGeometryShader(
	ID3D11Device *pDevice,
	const void *pShaderBytecode,
	SIZE_T BytecodeLength,
	ID3D11ClassLinkage *pClassLinkage,
	ID3D11GeometryShader **ppGeometryShader)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreateGeometryShader);
	const HRESULT hr = trampoline(pDevice, pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader);
	DumpShader(GS, pShaderBytecode, BytecodeLength, ppGeometryShader);
	return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreatePixelShader(
	ID3D11Device *pDevice,
	const void *pShaderBytecode,
	SIZE_T BytecodeLength,
	ID3D11ClassLinkage *pClassLinkage,
	ID3D11PixelShader **ppPixelShader)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreatePixelShader);
	const HRESULT hr = trampoline(pDevice, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
	DumpShader(PS, pShaderBytecode, BytecodeLength, ppPixelShader);
	return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11Device_CreateComputeShader(
	ID3D11Device *pDevice,
	const void *pShaderBytecode,
	SIZE_T BytecodeLength,
	ID3D11ClassLinkage *pClassLinkage,
	ID3D11ComputeShader **ppComputeShader)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11Device_CreateComputeShader);
	const HRESULT hr = trampoline(pDevice, pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader);
	DumpShader(CS, pShaderBytecode, BytecodeLength, ppComputeShader);
	return hr;
}

HRESULT STDMETHODCALLTYPE ID3D11DeviceContext_FinishCommandList(ID3D11DeviceContext *pDeviceContext, BOOL RestoreDeferredContextState,
	ID3D11CommandList **ppCommandList)
{
	static const auto trampoline = ReShade::Hooks::Call(&ID3D11DeviceContext_FinishCommandList);
	
	//LOG(INFO) << "Request to finish CL on context: " << pDeviceContext << " Restore: " << RestoreDeferredContextState ;

	deferredDrawMutex.lock();

	AllCLsMutex.lock();

	// Tell our metaCL we are done
	DefContext* ctx = AllDefContexts_Get(pDeviceContext);
	MetaCL* metaCL = nullptr;
	if (!ctx) LOG(ERROR) << "Deferred context not tracked when finishing a command list!";
	else metaCL = ctx->EndRecording();

	RestoreDeferredContextState = false;
	const HRESULT hr = trampoline(pDeviceContext, RestoreDeferredContextState, ppCommandList);

	//LOG(INFO) << "Trampoline result address " << *ppCommandList;
	if (!SUCCEEDED(hr)) {
		LOG(ERROR) << "Trampoline to CL finish failed. HR " << hr;
	}

	//LOG(INFO) << "Finish CL. Thread:  " << GetCurrentThreadId() << " Pointer: " << *ppCommandList;

	if (*ppCommandList) {

		ReShade::Hooks::Register(VTABLE(*ppCommandList)[2], reinterpret_cast<ReShade::Hook::Function>(&ID3D11CommandList_Release));

		{
			auto result = AllCLs.insert(*ppCommandList);
			if (!result.second) {
				//Dumb sync issue, the release is arriving right after
				LOG(INFO) << "Warning Adding a CL to track which was already tracked! " << *ppCommandList;

				// Another CL with the same pointer value is currently being released by another thread
				AllMetaCLs_SafeRemove(*ppCommandList);

				//TODO terminate CL
				RecentCLErrors.insert(*ppCommandList);
			}

			AllMetaCLs_SafeAdd(*ppCommandList, metaCL);
		}
	}
	AllCLsMutex.unlock();

	deferredDrawMutex.unlock();

	return hr;
}

static ID3D11Texture2D* CreateTex2D(ID3D11Device* device, int width, int height, DXGI_FORMAT format) {
	D3D11_TEXTURE2D_DESC copydesc;
	ZeroMemory(&copydesc, sizeof(D3D11_TEXTURE2D_DESC));
	copydesc.Width = width;
	copydesc.Height = height;
	copydesc.ArraySize = 1;
	copydesc.MipLevels = 1;
	copydesc.Format = format;
	copydesc.SampleDesc.Count = 1;
	copydesc.SampleDesc.Quality = 0;
	copydesc.Usage = D3D11_USAGE_STAGING;
	copydesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ID3D11Texture2D *textureStaging = nullptr;
	HRESULT hr = device->CreateTexture2D(&copydesc, nullptr, &textureStaging);

	if (FAILED(hr))
	{
		LOG(TRACE) << "Failed to create texture for pool! HRESULT is '" << hr << "'.";
		return nullptr;
	}

	return textureStaging;
}

// D3D11
EXPORT HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
{
	LOG(INFO) << "Redirecting '" << "D3D11CreateDevice" << "(" << pAdapter << ", " << DriverType << ", " << Software << ", " << Flags << ", " << pFeatureLevels << ", " << FeatureLevels << ", " << SDKVersion << ", " << ppDevice << ", " << pFeatureLevel << ", " << ppImmediateContext << ")' ...";
	LOG(INFO) << "> Passing on to 'D3D11CreateDeviceAndSwapChain':";

	return D3D11CreateDeviceAndSwapChain(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, nullptr, nullptr, ppDevice, pFeatureLevel, ppImmediateContext);
}
EXPORT HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
{
	LOG(INFO) << "Redirecting '" << "D3D11CreateDeviceAndSwapChain" << "(" << pAdapter << ", " << DriverType << ", " << Software << ", " << Flags << ", " << pFeatureLevels << ", " << FeatureLevels << ", " << SDKVersion << ", " << pSwapChainDesc << ", " << ppSwapChain << ", " << ppDevice << ", " << pFeatureLevel << ", " << ppImmediateContext << ")' ...";

#ifdef _DEBUG
	Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	ID3D11DeviceContext *pImmediateContext = nullptr;

	//if (pSwapChainDesc) LOG(INFO) << "SwapChain: " << pSwapChainDesc->BufferDesc.Width << "x" << pSwapChainDesc->BufferDesc.Height;

	const HRESULT hr = ReShade::Hooks::Call(&D3D11CreateDeviceAndSwapChain)(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, &pImmediateContext);

	if (ppImmediateContext != nullptr)
	{
		*ppImmediateContext = pImmediateContext;
	}

	//Populate our pool of RTs
	for (int i = 0; i < POOL_720P_COUNT; i++)
	{
		ID3D11Texture2D* text = CreateTex2D(*ppDevice, BACK_BUFFER_WIDTH, BACK_BUFFER_HEIGHT, DXGI_FORMAT_B8G8R8A8_TYPELESS);
		if (text) {
			Pool_RGB8_Typeless_720p.push_back(text);
			LOG(INFO) << "Added scratch texture " << text;
		}
	}

	if (SUCCEEDED(hr) && (ppDevice != nullptr && *ppDevice != nullptr && pImmediateContext != nullptr))
	{
		ReShade::Hooks::Register(VTABLE(*ppDevice)[9], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreateRenderTargetView));
		ReShade::Hooks::Register(VTABLE(*ppDevice)[10], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreateDepthStencilView));
		ReShade::Hooks::Register(VTABLE(*ppDevice)[12], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreateVertexShader));
		ReShade::Hooks::Register(VTABLE(*ppDevice)[13], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreateGeometryShader));
		ReShade::Hooks::Register(VTABLE(*ppDevice)[15], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreatePixelShader));
		ReShade::Hooks::Register(VTABLE(*ppDevice)[18], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreateComputeShader));
		ReShade::Hooks::Register(VTABLE(*ppDevice)[27], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CreateDeferredContext));
		//ReShade::Hooks::Register(VTABLE(*ppDevice)[33], reinterpret_cast<ReShade::Hook::Function>(&ID3D11Device_CheckFeatureSupport));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[12], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_DrawIndexed));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[13], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_Draw));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[20], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_DrawIndexedInstanced));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[21], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_DrawInstanced));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[33], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_OMSetRenderTargets));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[34], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_OMSetRenderTargetsAndUnorderedAccessViews));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[47], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_CopyResource));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[53], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_ClearDepthStencilView));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[89], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_OMGetRenderTargets));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[90], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews));

		ReShade::Hooks::Register(VTABLE(pImmediateContext)[58], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_ExecuteCommandList));
		ReShade::Hooks::Register(VTABLE(pImmediateContext)[114], reinterpret_cast<ReShade::Hook::Function>(&ID3D11DeviceContext_FinishCommandList));
	}

	return hr;
}

/*

D3D11 Context VTable

Index: 0 | QueryInterface
Index: 1 | Addref
Index: 2 | Release
Index: 3 | GetDevice
Index: 4 | GetPrivateData
Index: 5 | SetPrivateData
Index: 6 | SetPrivateDataInterface
Index: 7 | VSSetConstantBuffers
Index: 8 | PSSetShaderResources
Index: 9 | PSSetShader
Index: 10 | SetSamplers
Index: 11 | SetShader
Index: 12 | DrawIndexed
Index: 13 | Draw
Index: 14 | Map
Index: 15 | Unmap
Index: 16 | PSSetConstantBuffer
Index: 17 | IASetInputLayout
Index: 18 | IASetVertexBuffers
Index: 19 | IASetIndexBuffer
Index: 20 | DrawIndexedInstanced
Index: 21 | DrawInstanced
Index: 22 | GSSetConstantBuffers
Index: 23 | GSSetShader
Index: 24 | IASetPrimitiveTopology
Index: 25 | VSSetShaderResources
Index: 26 | VSSetSamplers
Index: 27 | Begin
Index: 28 | End
Index: 29 | GetData
Index: 30 | GSSetPredication
Index: 31 | GSSetShaderResources
Index: 32 | GSSetSamplers
Index: 33 | OMSetRenderTargets
Index: 34 | OMSetRenderTargetsAndUnorderedAccessViews
Index: 35 | OMSetBlendState
Index: 36 | OMSetDepthStencilState
Index: 37 | SOSetTargets
Index: 38 | DrawAuto
Index: 39 | DrawIndexedInstancedIndirect
Index: 40 | DrawInstancedIndirect
Index: 41 | Dispatch
Index: 42 | DispatchIndirect
Index: 43 | RSSetState
Index: 44 | RSSetViewports
Index: 45 | RSSetScissorRects
Index: 46 | CopySubresourceRegion
Index: 47 | CopyResource
Index: 48 | UpdateSubresource
Index: 49 | CopyStructureCount
Index: 50 | ClearRenderTargetView
Index: 51 | ClearUnorderedAccessViewUint
Index: 52 | ClearUnorderedAccessViewFloat
Index: 53 | ClearDepthStencilView
Index: 54 | GenerateMips
Index: 55 | SetResourceMinLOD
Index: 56 | GetResourceMinLOD
Index: 57 | ResolveSubresource
Index: 58 | ExecuteCommandList
Index: 59 | HSSetShaderResources
Index: 60 | HSSetShader
Index: 61 | HSSetSamplers
Index: 62 | HSSetConstantBuffers
Index: 63 | DSSetShaderResources
Index: 64 | DSSetShader
Index: 65 | DSSetSamplers
Index: 66 | DSSetConstantBuffers
Index: 67 | DSSetShaderResources
Index: 68 | CSSetUnorderedAccessViews
Index: 69 | CSSetShader
Index: 70 | CSSetSamplers
Index: 71 | CSSetConstantBuffers
Index: 72 | VSGetConstantBuffers
Index: 73 | PSGetShaderResources
Index: 74 | PSGetShader
Index: 75 | PSGetSamplers
Index: 76 | VSGetShader
Index: 77 | PSGetConstantBuffers
Index: 78 | IAGetInputLayout
Index: 79 | IAGetVertexBuffers
Index: 80 | IAGetIndexBuffer
Index: 81 | GSGetConstantBuffers
Index: 82 | GSGetShader
Index: 83 | IAGetPrimitiveTopology
Index: 84 | VSGetShaderResources
Index: 85 | VSGetSamplers
Index: 86 | GetPredication
Index: 87 | GSGetShaderResources
Index: 88 | GSGetSamplers
Index: 89 | OMGetRenderTargets
Index: 90 | OMGetRenderTargetsAndUnorderedAccessViews
Index: 91 | OMGetBlendState
Index: 92 | OMGetDepthStencilState
Index: 93 | SOGetTargets
Index: 94 | RSGetState
Index: 95 | RSGetViewports
Index: 96 | RSGetScissorRects
Index: 97 | HSGetShaderResources
Index: 98 | HSGetShader
Index: 99 | HSGetSamplers
Index: 100 | HSGetConstantBuffers
Index: 101 | DSGetShaderResources
Index: 102 | DSGetShader
Index: 103 | DSGetSamplers
Index: 104 | DSGetConstantBuffers
Index: 105 | CSGetShaderResources
Index: 106 | CSGetUnorderedAccessViews
Index: 107 | CSGetShader
Index: 108 | CSGetSamplers
Index: 109 | CSGetConstantBuffers
Index: 110 | ClearState
Index: 111 | Flush
Index: 112 | GetType
Index: 113 | GetContextFlags
Index: 114 | FinishCommandList

D3D11 Device VTable
*QueryInterface 						0
*AddRef 								1
*Release 								2
*CreateBuffer 							3
*CreateTexture1D 						4
*CreateTexture2D 						5
*CreateTexture3D 						6
*CreateShaderResourceView				7
*CreateUnorderedAccessView 				8
*CreateRenderTargetView 				9
*CreateDepthStencilView 				10
*CreateInputLayout 						11
*CreateVertexShader 					12
*CreateGeometryShader 					13
*CreateGeometryShaderWithStreamOutput	14
*CreatePixelShader 						15
*CreateHullShader 						16
*CreateDomainShader						17
*CreateComputeShader					18
*CreateClassLinkage 					19
*CreateBlendState 						20
*CreateDepthStencilState 				21
*CreateRasterizerState 					22
*CreateSamplerState						23
*CreateQuery 							24
*CreatePredicate 						25
*CreateCounter 							26
*CreateDeferredContext 					27
*OpenSharedResource 					28
*CheckFormatSupport 					29
*CheckMultisampleQualityLevels 			30
*CheckCounterInfo 						31
*CheckCounter 							32
*CheckFeatureSupport 					33
*GetPrivateData 						34
*SetPrivateData 						35
*SetPrivateDataInterface 				36
*GetFeatureLevel 						37
*GetCreationFlags 						38
*GetDeviceRemovedReason 				39
*GetImmediateContext 					40
*SetExceptionMode 						41
*GetExceptionMode 						42


// IDXGI SWAPCHAIN virtuals
[0]    7405CADA    (CMTUseCountedObject<CDXGISwapChain>::QueryInterface)
[1]    7405C9A7    (CMTUseCountedObject<CDXGISwapChain>::AddRef)
[2]    7405C9D8    (CMTUseCountedObject<CDXGISwapChain>::Release)
[3]    7405D6BF    (CDXGISwapChain::SetPrivateData)
[4]    7405F6FC    (CDXGISwapChain::SetPrivateDataInterface)
[5]    7405D6AF    (CDXGISwapChain::GetPrivateData)
[6]    7406106A    (CDXGISwapChain::GetParent)
[7]    7405EFDE    (CDXGISwapChain::GetDevice)
[8]    74061BD1    (CDXGISwapChain::Present)
[9]    740617A7    (CDXGISwapChain::GetBuffer)
[10]    74065CD6    (CDXGISwapChain::SetFullscreenState)
[11]    740662DC    (CDXGISwapChain::GetFullscreenState)
[12]    74061146    (CDXGISwapChain::GetDesc)
[13]    740655ED    (CDXGISwapChain::ResizeBuffers)
[14]    74065B8D    (CDXGISwapChain::ResizeTarget)
[15]    7406197B    (CDXGISwapChain::GetContainingOutput)
[16]    74066524    (CDXGISwapChain::GetFrameStatistics)
[17]    74066A58    (CDXGISwapChain::GetLastPresentCount)
[18]    740612C6    (CDXGISwapChain::GetDesc1)
[19]    740613E0    (CDXGISwapChain::GetFullscreenDesc)
[20]    740614F9    (CDXGISwapChain::GetHwnd)
[21]    7406156D    (CDXGISwapChain::GetCoreWindow)
[22]    74061D0D    (CDXGISwapChain[::IDXGISwapChain1]::Present1)
[23]    74062069    (CDXGISwapChain::IsTemporaryMonoSupported)
[24]    740615BB    (CDXGISwapChain::GetRestrictToOutput)
[25]    740615FB    (CDXGISwapChain::SetBackgroundColor)
[26]    740616F1    (CDXGISwapChain::GetBackgroundColor)
[27]    7406173F    (CDXGISwapChain::SetRotation)
[28]    74061770    (CDXGISwapChain::GetRotation)
[29]    7405CC1A    (CMTUseCountedObject<CDXGISwapChain>::`vector deleting destructor')
[30]    7405181E    (CMTUseCountedObject<CDXGISwapChain>::LUCCompleteLayerConstruction)
[31]    7405CBA5    (DXGID3D10ETWRundown)
// DXGI VTable:
[0]	6ED3F979	(CMTUseCountedObject<CDXGISwapChain>::QueryInterface)
[1]	6ED3F84D	(CMTUseCountedObject<CDXGISwapChain>::AddRef)
[2]	6ED3F77D	(CMTUseCountedObject<CDXGISwapChain>::Release)
[3]	6ED6A6D7	(CDXGISwapChain::SetPrivateData)
[4]	6ED6A904	(CDXGISwapChain::SetPrivateDataInterface)
[5]	6ED72BC9	(CDXGISwapChain::GetPrivateData)
[6]	6ED6DCDD	(CDXGISwapChain::GetParent)
[7]	6ED69BF4	(CDXGISwapChain::GetDevice)
[8]	6ED3FAAD	(CDXGISwapChain::Present)
[9]	6ED40209	(CDXGISwapChain::GetBuffer)
[10]	6ED47C1C	(CDXGISwapChain::SetFullscreenState)
[11]	6ED48CD9	(CDXGISwapChain::GetFullscreenState)
[12]	6ED40CB1	(CDXGISwapChain::GetDesc)
[13]	6ED48A3B	(CDXGISwapChain::ResizeBuffers)
[14]	6ED6F153	(CDXGISwapChain::ResizeTarget)
[15]	6ED47BA5	(CDXGISwapChain::GetContainingOutput)
[16]	6ED6D9B5	(CDXGISwapChain::GetFrameStatistics)
[17]	6ED327B5	(CDXGISwapChain::GetLastPresentCount)
[18]	6ED43400	(CDXGISwapChain::GetDesc1)
[19]	6ED6D9D0	(CDXGISwapChain::GetFullscreenDesc)
[20]	6ED6DA90	(CDXGISwapChain::GetHwnd)
[21]	6ED6D79F	(CDXGISwapChain::GetCoreWindow)
[22]	6ED6E352	(?Present1@?QIDXGISwapChain2@@CDXGISwapChain@@UAGJIIPBUDXGI_PRESENT_PARAMETERS@@@Z)
[23]	6ED6E240	(CDXGISwapChain::IsTemporaryMonoSupported)
[24]	6ED44146	(CDXGISwapChain::GetRestrictToOutput)
[25]	6ED6F766	(CDXGISwapChain::SetBackgroundColor)
[26]	6ED6D6B9	(CDXGISwapChain::GetBackgroundColor)
[27]	6ED4417B	(CDXGISwapChain::SetRotation)
[28]	6ED6DDE3	(CDXGISwapChain::GetRotation)
[29]	6ED6FF85	(CDXGISwapChain::SetSourceSize)
[30]	6ED6DF4F	(CDXGISwapChain::GetSourceSize)
[31]	6ED6FCBD	(CDXGISwapChain::SetMaximumFrameLatency)
[32]	6ED6DBE5	(CDXGISwapChain::GetMaximumFrameLatency)
[33]	6ED6D8CD	(CDXGISwapChain::GetFrameLatencyWaitableObject)
[34]	6ED6FB45	(CDXGISwapChain::SetMatrixTransform)
[35]	6ED6DAD0	(CDXGISwapChain::GetMatrixTransform)
[36]	6ED6C155	(CDXGISwapChain::CheckMultiplaneOverlaySupportInternal)
[37]	6ED6E82D	(CDXGISwapChain::PresentMultiplaneOverlayInternal)
[38]	6ED4397A	(CMTUseCountedObject<CDXGISwapChain>::`vector deleting destructor')
[39]	6ED4EAE0	(CSwapBuffer::AddRef)
[40]	6ED46C81	(CMTUseCountedObject<CDXGISwapChain>::LUCBeginLayerDestruction)
*/