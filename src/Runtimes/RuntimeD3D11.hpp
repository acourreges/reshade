#pragma once

#include "Runtime.hpp"

#include <d3d11_1.h>
#include <set>

namespace ReShade { namespace Runtimes
{
	struct D3D11Runtime : public Runtime, public std::enable_shared_from_this<D3D11Runtime>
	{
		struct DepthSourceInfo
		{
			UINT Width, Height;
			FLOAT DrawCallCount, DrawVerticesCount;
		};

		D3D11Runtime(ID3D11Device *device, IDXGISwapChain *swapchain);
		~D3D11Runtime();

		bool OnCreateInternal(const DXGI_SWAP_CHAIN_DESC &desc);
		void OnDeleteInternal();
		void OnDrawInternal(ID3D11DeviceContext *context, unsigned int vertices);
		void OnPresentInternal();
		void OnGetBackBuffer(ID3D11Texture2D *&buffer);
		void OnCreateDepthStencilView(ID3D11Resource *resource, ID3D11DepthStencilView *depthstencil);
		void OnDeleteDepthStencilView(ID3D11DepthStencilView *depthstencil);
		void OnSetDepthStencilView(ID3D11DepthStencilView *&depthstencil);
		void OnGetDepthStencilView(ID3D11DepthStencilView *&depthstencil);
		void OnClearDepthStencilView(ID3D11DepthStencilView *&depthstencil);
		void OnCopyResource(ID3D11Resource *&dest, ID3D11Resource *&source);

		void DetectDepthSource();
		bool CreateBackBufferReplacement(ID3D11Texture2D *backbuffer, const DXGI_SAMPLE_DESC &samples);
		bool CreateDepthStencilReplacement(ID3D11DepthStencilView *depthstencil);

		virtual std::unique_ptr<Effect> CompileEffect(const EffectTree &ast, std::string &errors) const override;
		virtual void CreateScreenshot(unsigned char *buffer, std::size_t size) const override;
		virtual void DumpFrameTrace(const boost::filesystem::path &path) override;
		virtual void ToggleDebugView(bool saveCurrent, bool playSave, bool playNext) override;
		static bool mIsDumpingTrace;
		static int mToggleDebugViewID;
		boost::filesystem::path DumpRootPath;
		void DumpTexture2D(boost::filesystem::path rootPathNoExt, ID3D11Texture2D* texture);
		void DumpReadableTexture2D(boost::filesystem::path rootPathNoExt, ID3D11Texture2D* texture);
		void OnSetMRT(UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView);
		void OnExecuteCommandList(ID3D11DeviceContext *context, ID3D11CommandList *pCommandList, BOOL RestoreContextState);

		int lastMRTsCount;
		ID3D11RenderTargetView* lastMRTs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
		ID3D11DepthStencilView* lastDepthStencil;
		DWORD CLThread;

		static std::vector<ID3D11RenderTargetView*> WatchedRTs;
		void AddWatchRT(ID3D11RenderTargetView* rt) {
			WatchedRTs.push_back(rt);
			LOG(INFO) << "Added RT to watch " << rt << ". Total alive: " << WatchedRTs.size();
		}
		void RemoveWatchRT(ID3D11RenderTargetView* rt) {
			LOG(INFO) << "RT released: " << rt;
			//int deleted = WatchedRTs.erase(rt);
			//if (deleted == 0) LOG(ERROR) << "Cannot remove a RT we were not tracking: " << rt;
			auto it = std::find(WatchedRTs.begin(), WatchedRTs.end(), rt);
			if (it != WatchedRTs.end()) WatchedRTs.erase(it);
		}

		ID3D11Device *mDevice;
		ID3D11DeviceContext *mImmediateContext;
		IDXGISwapChain *mSwapChain;
		DXGI_SWAP_CHAIN_DESC mSwapChainDesc;
		std::unique_ptr<class D3D11StateBlock> mStateBlock;
		ID3D11Texture2D *mBackBuffer, *mBackBufferReplacement;
		ID3D11Texture2D *mBackBufferTexture;
		ID3D11ShaderResourceView *mBackBufferTextureSRV[2];
		ID3D11RenderTargetView *mBackBufferTargets[2];
		ID3D11DepthStencilView *mDepthStencil, *mDepthStencilReplacement;
		ID3D11Texture2D *mDepthStencilTexture;
		ID3D11ShaderResourceView *mDepthStencilTextureSRV;
		ID3D11DepthStencilView *mDefaultDepthStencil;
		std::unordered_map<ID3D11DepthStencilView *, DepthSourceInfo> mDepthSourceTable;
		CRITICAL_SECTION mCS;
		bool mLost;
	};

	struct D3D11Effect : public Effect
	{
		friend struct D3D11Runtime;
		friend struct D3D11Texture;

		D3D11Effect(std::shared_ptr<const D3D11Runtime> runtime);
		~D3D11Effect();

		inline bool AddTexture(const std::string &name, Texture *texture)
		{
			return this->mTextures.emplace(name, std::unique_ptr<Texture>(texture)).second;
		}
		inline bool AddConstant(const std::string &name, Constant *constant)
		{
			return this->mConstants.emplace(name, std::unique_ptr<Constant>(constant)).second;
		}
		inline bool AddTechnique(const std::string &name, Technique *technique)
		{
			return this->mTechniques.emplace(name, std::unique_ptr<Technique>(technique)).second;
		}

		virtual void Begin() const override;
		virtual void End() const override;

		std::shared_ptr<const D3D11Runtime> mRuntime;
		ID3D11RasterizerState *mRasterizerState;
		std::vector<ID3D11SamplerState *> mSamplerStates;
		std::vector<ID3D11ShaderResourceView *> mShaderResources;
		std::vector<ID3D11Buffer *> mConstantBuffers;
		std::vector<unsigned char *> mConstantStorages;
		mutable bool mConstantsDirty;
	};
	struct D3D11Texture : public Effect::Texture
	{
		enum class Source
		{
			Memory,
			BackBuffer,
			DepthStencil
		};

		D3D11Texture(D3D11Effect *effect, const Description &desc);
		~D3D11Texture();

		inline bool AddAnnotation(const std::string &name, const Effect::Annotation &value)
		{
			return this->mAnnotations.emplace(name, value).second;
		}

		virtual bool Update(unsigned int level, const unsigned char *data, std::size_t size) override;
		void ChangeSource(ID3D11ShaderResourceView *srv, ID3D11ShaderResourceView *srvSRGB);

		D3D11Effect *mEffect;
		Source mSource;
		std::size_t mRegister;
		ID3D11Texture2D *mTexture;
		ID3D11ShaderResourceView *mShaderResourceView[2];
		ID3D11RenderTargetView *mRenderTargetView[2];
	};
	struct D3D11Constant : public Effect::Constant
	{
		D3D11Constant(D3D11Effect *effect, const Description &desc);
		~D3D11Constant();

		inline bool AddAnnotation(const std::string &name, const Effect::Annotation &value)
		{
			return this->mAnnotations.emplace(name, value).second;
		}

		virtual void GetValue(unsigned char *data, std::size_t size) const override;
		virtual void SetValue(const unsigned char *data, std::size_t size) override;

		D3D11Effect *mEffect;
		std::size_t mBufferIndex, mBufferOffset;
	};
	struct D3D11Technique : public Effect::Technique
	{
		struct Pass
		{
			ID3D11VertexShader *VS;
			ID3D11PixelShader *PS;
			ID3D11BlendState *BS;
			ID3D11DepthStencilState *DSS;
			UINT StencilRef;
			ID3D11RenderTargetView *RT[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
			ID3D11ShaderResourceView *RTSRV[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
			D3D11_VIEWPORT Viewport;
			std::vector<ID3D11ShaderResourceView *> SRV;
		};

		D3D11Technique(D3D11Effect *effect, const Description &desc);
		~D3D11Technique();

		inline bool AddAnnotation(const std::string &name, const Effect::Annotation &value)
		{
			return this->mAnnotations.emplace(name, value).second;
		}
		inline void AddPass(const Pass &pass)
		{
			this->mPasses.push_back(pass);
		}

		virtual void RenderPass(unsigned int index) const override;

		D3D11Effect *mEffect;
		std::vector<Pass> mPasses;
	};
} }