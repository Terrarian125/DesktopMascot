#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#pragma comment(lib, "dcomp.lib")
#include "Direct3D.h"
#include "Global.h"
#include "Transform.h"

//画面の描画に関する処理
namespace Direct3D
{
	//【スワップチェーン】
	//画用紙を2枚用紙しておき、片方を画面に映している間にもう一方に描画。
	//1フレーム分の絵が出来上がったら画用紙を交換。これにより画面がちらつかない。
	//その辺を司るのがスワップチェーン
	IDXGISwapChain*         pSwapChain_ = nullptr;

	//【レンダーターゲットビュー】
	//描画したいものと、描画先（上でいう画用紙）の橋渡しをするもの
	ID3D11RenderTargetView* pRenderTargetView_ = nullptr;
	IDCompositionDevice* pCompositionDevice_ = nullptr;
	IDCompositionTarget* pCompositionTarget_ = nullptr;
	IDCompositionVisual* pCompositionVisual_ = nullptr;

	//【デプスステンシル】
	//Zバッファ法を用いて、3D物体の前後関係を正しく表示するためのもの
	ID3D11Texture2D*		pDepthStencil = nullptr;

	//【デプスステンシルビュー】
	//デプスステンシルの情報をシェーダーに渡すためのもの
	ID3D11DepthStencilView* pDepthStencilView = nullptr;
	ID3D11DepthStencilState* pDepthStencilState[BLEND_MAX];

	//【ブレンドステート】
	//半透明のものをどのように表現するか
	ID3D11BlendState*	pBlendState[BLEND_MAX];

	bool		isDrawCollision_ = true;	//コリジョンを表示するか
	bool		_isLighting = false;		//ライティングするか



	//extern宣言した変数の初期化
	ID3D11Device*           pDevice_ = nullptr;
	ID3D11DeviceContext*    pContext_ = nullptr;
	SHADER_BUNDLE			shaderBundle[SHADER_MAX] = { 0 };
	int						screenWidth_ = 0;
	int						screenHeight_ = 0;
	const char* initializationFailureStage_ = "D3D11 device";



	//初期化処理
	HRESULT Direct3D::Initialize(HWND hWnd, int screenWidth, int screenHeight)
	{
		initializationFailureStage_ = "D3D11 device";
		D3D_FEATURE_LEVEL level;
		const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
			&pDevice_, &level, &pContext_);
		if (FAILED(hr)) return hr;

		IDXGIDevice* dxgiDevice = nullptr;
		IDXGIAdapter* adapter = nullptr;
		IDXGIFactory2* factory = nullptr;
		IDXGISwapChain1* compositionSwapChain = nullptr;
		initializationFailureStage_ = "DXGI device";
		hr = pDevice_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DXGI adapter";
			hr = dxgiDevice->GetAdapter(&adapter);
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DXGI factory";
			hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
		}

		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
		swapChainDesc.Width = static_cast<UINT>(screenWidth);
		swapChainDesc.Height = static_cast<UINT>(screenHeight);
		swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		swapChainDesc.SampleDesc.Count = 1;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = 2;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
		swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DXGI composition swap chain";
			hr = factory->CreateSwapChainForComposition(dxgiDevice, &swapChainDesc, nullptr, &compositionSwapChain);
		}
		if (SUCCEEDED(hr))
		{
			pSwapChain_ = compositionSwapChain;
			pSwapChain_->AddRef();
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DirectComposition device";
			hr = DCompositionCreateDevice(dxgiDevice, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(&pCompositionDevice_));
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DirectComposition HWND target";
			hr = pCompositionDevice_->CreateTargetForHwnd(hWnd, TRUE, &pCompositionTarget_);
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DirectComposition visual";
			hr = pCompositionDevice_->CreateVisual(&pCompositionVisual_);
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DirectComposition content";
			hr = pCompositionVisual_->SetContent(compositionSwapChain);
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DirectComposition root";
			hr = pCompositionTarget_->SetRoot(pCompositionVisual_);
		}
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "DirectComposition commit";
			hr = pCompositionDevice_->Commit();
		}
		SAFE_RELEASE(compositionSwapChain);
		SAFE_RELEASE(factory);
		SAFE_RELEASE(adapter);
		SAFE_RELEASE(dxgiDevice);
		if (FAILED(hr)) return hr;

		initializationFailureStage_ = "Back buffer";
		ID3D11Texture2D* pBackBuffer = nullptr;
		hr = pSwapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pBackBuffer));
		if (FAILED(hr)) return hr;
		hr = pDevice_->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView_);
		SAFE_RELEASE(pBackBuffer);
		if (FAILED(hr)) return hr;

		D3D11_VIEWPORT vp = {};
		vp.Width = static_cast<float>(screenWidth);
		vp.Height = static_cast<float>(screenHeight);
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		InitShaderBundle();
		Direct3D::SetShader(Direct3D::SHADER_3D);

		D3D11_TEXTURE2D_DESC descDepth = {};
		descDepth.Width = screenWidth;
		descDepth.Height = screenHeight;
		descDepth.MipLevels = 1;
		descDepth.ArraySize = 1;
		descDepth.Format = DXGI_FORMAT_D32_FLOAT;
		descDepth.SampleDesc.Count = 1;
		descDepth.Usage = D3D11_USAGE_DEFAULT;
		descDepth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		initializationFailureStage_ = "Depth texture";
		hr = pDevice_->CreateTexture2D(&descDepth, nullptr, &pDepthStencil);
		if (SUCCEEDED(hr))
		{
			initializationFailureStage_ = "Depth view";
			hr = pDevice_->CreateDepthStencilView(pDepthStencil, nullptr, &pDepthStencilView);
		}
		if (FAILED(hr)) return hr;

		{
			initializationFailureStage_ = "Depth stencil state";
			D3D11_DEPTH_STENCIL_DESC desc = {};
			desc.DepthEnable = true;
			desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			desc.StencilEnable = false;
			hr = pDevice_->CreateDepthStencilState(&desc, &pDepthStencilState[BLEND_DEFAULT]);
			if (FAILED(hr)) return hr;
			pContext_->OMSetDepthStencilState(pDepthStencilState[BLEND_DEFAULT], 0);
			desc.StencilEnable = false;
			hr = pDevice_->CreateDepthStencilState(&desc, &pDepthStencilState[BLEND_ADD]);
			if (FAILED(hr)) return hr;
		}

		{
			initializationFailureStage_ = "Blend state";
			D3D11_BLEND_DESC blendDesc = {};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			hr = pDevice_->CreateBlendState(&blendDesc, &pBlendState[BLEND_DEFAULT]);
			if (FAILED(hr)) return hr;
			const float blendFactor[4] = {};
			pContext_->OMSetBlendState(pBlendState[BLEND_DEFAULT], blendFactor, 0xffffffff);
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
			hr = pDevice_->CreateBlendState(&blendDesc, &pBlendState[BLEND_ADD]);
			if (FAILED(hr)) return hr;
		}

		pContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		pContext_->OMSetRenderTargets(1, &pRenderTargetView_, pDepthStencilView);
		pContext_->RSSetViewports(1, &vp);
		isDrawCollision_ = GetPrivateProfileInt("DEBUG", "ViewCollider", 0, ".\\setup.ini") != 0;
		screenWidth_ = screenWidth;
		screenHeight_ = screenHeight;
		initializationFailureStage_ = "";
		return S_OK;
	}

	const char* GetInitializationFailureStage()
	{
		return initializationFailureStage_;
	}
void InitShaderBundle()
	{
		DWORD vectorSize = sizeof(XMFLOAT3);

		//3D
		{
			// 頂点シェーダの作成（コンパイル）
			ID3DBlob *pCompileVS = NULL;
			D3DCompileFromFile(L"Shader/Simple3D.hlsl", nullptr, nullptr, "VS", "vs_5_0", NULL, 0, &pCompileVS, NULL);
			pDevice_->CreateVertexShader(pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), NULL, &shaderBundle[SHADER_3D].pVertexShader);


			// ピクセルシェーダの作成（コンパイル）
			ID3DBlob *pCompilePS = NULL;
			D3DCompileFromFile(L"Shader/Simple3D.hlsl", nullptr, nullptr, "PS", "ps_5_0", NULL, 0, &pCompilePS, NULL);
			pDevice_->CreatePixelShader(pCompilePS->GetBufferPointer(), pCompilePS->GetBufferSize(), NULL, &shaderBundle[SHADER_3D].pPixelShader);


			// 頂点レイアウトの作成（1頂点の情報が何のデータをどんな順番で持っているか）
			D3D11_INPUT_ELEMENT_DESC layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, vectorSize * 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },	//頂点位置
				{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, vectorSize * 1, D3D11_INPUT_PER_VERTEX_DATA, 0 },	//法線
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, vectorSize * 2, D3D11_INPUT_PER_VERTEX_DATA, 0 },	//テクスチャ（UV）座標
			};
			pDevice_->CreateInputLayout(layout, 3, pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), &shaderBundle[SHADER_3D].pVertexLayout);


			//シェーダーが無事作成できたので、コンパイルしたやつはいらない
			pCompileVS->Release();
			pCompilePS->Release();

			//ラスタライザ作成
			D3D11_RASTERIZER_DESC rdc = {};
			rdc.CullMode = D3D11_CULL_BACK;
			rdc.FillMode = D3D11_FILL_SOLID;
			rdc.FrontCounterClockwise = FALSE;	//反時計回りは表面じゃない
			pDevice_->CreateRasterizerState(&rdc, &shaderBundle[SHADER_3D].pRasterizerState);
		}


		//2D
		{
			// 頂点シェーダの作成（コンパイル）
			ID3DBlob *pCompileVS = NULL;
			D3DCompileFromFile(L"Shader/Simple2D.hlsl", nullptr, nullptr, "VS", "vs_5_0", NULL, 0, &pCompileVS, NULL);
			pDevice_->CreateVertexShader(pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), NULL, &shaderBundle[SHADER_2D].pVertexShader);


			// ピクセルシェーダの作成（コンパイル）
			ID3DBlob *pCompilePS = NULL;
			D3DCompileFromFile(L"Shader/Simple2D.hlsl", nullptr, nullptr, "PS", "ps_5_0", NULL, 0, &pCompilePS, NULL);
			pDevice_->CreatePixelShader(pCompilePS->GetBufferPointer(), pCompilePS->GetBufferSize(), NULL, &shaderBundle[SHADER_2D].pPixelShader);


			// 頂点レイアウトの作成（1頂点の情報が何のデータをどんな順番で持っているか）
			D3D11_INPUT_ELEMENT_DESC layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, vectorSize * 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, vectorSize * 1, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			pDevice_->CreateInputLayout(layout, 2, pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), &shaderBundle[SHADER_2D].pVertexLayout);


			//シェーダーが無事作成できたので、コンパイルしたやつはいらない
			pCompileVS->Release();
			pCompilePS->Release();

			//ラスタライザ作成
			D3D11_RASTERIZER_DESC rdc = {};
			rdc.CullMode = D3D11_CULL_BACK;
			rdc.FillMode = D3D11_FILL_SOLID;
			rdc.FrontCounterClockwise = TRUE;
			pDevice_->CreateRasterizerState(&rdc, &shaderBundle[SHADER_2D].pRasterizerState);
		}

		//DEBUG用
		{
			// 頂点シェーダの作成（コンパイル）
			ID3DBlob *pCompileVS = NULL;
			D3DCompileFromFile(L"Shader/Debug3D.hlsl", nullptr, nullptr, "VS", "vs_5_0", NULL, 0, &pCompileVS, NULL);
			pDevice_->CreateVertexShader(pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), NULL, &shaderBundle[SHADER_UNLIT].pVertexShader);


			// ピクセルシェーダの作成（コンパイル）
			ID3DBlob *pCompilePS = NULL;
			D3DCompileFromFile(L"Shader/Debug3D.hlsl", nullptr, nullptr, "PS", "ps_5_0", NULL, 0, &pCompilePS, NULL);
			pDevice_->CreatePixelShader(pCompilePS->GetBufferPointer(), pCompilePS->GetBufferSize(), NULL, &shaderBundle[SHADER_UNLIT].pPixelShader);


			// 頂点レイアウトの作成（1頂点の情報が何のデータをどんな順番で持っているか）
			D3D11_INPUT_ELEMENT_DESC layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			pDevice_->CreateInputLayout(layout, 1, pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), &shaderBundle[SHADER_UNLIT].pVertexLayout);


			//シェーダーが無事作成できたので、コンパイルしたやつはいらない
			pCompileVS->Release();
			pCompilePS->Release();

			//ラスタライザ作成
			D3D11_RASTERIZER_DESC rdc = {};
			rdc.CullMode = D3D11_CULL_NONE;
			rdc.FillMode = D3D11_FILL_WIREFRAME;
			rdc.FrontCounterClockwise = TRUE;
			pDevice_->CreateRasterizerState(&rdc, &shaderBundle[SHADER_UNLIT].pRasterizerState);
		}

		//BillBoard
		{
			// 頂点シェーダの作成（コンパイル）
			ID3DBlob* pCompileVS = NULL;
			D3DCompileFromFile(L"Shader/BillBoard.hlsl", nullptr, nullptr, "VS", "vs_5_0", NULL, 0, &pCompileVS, NULL);
			pDevice_->CreateVertexShader(pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), NULL, &shaderBundle[SHADER_BILLBOARD].pVertexShader);


			// ピクセルシェーダの作成（コンパイル）
			ID3DBlob* pCompilePS = NULL;
			D3DCompileFromFile(L"Shader/BillBoard.hlsl", nullptr, nullptr, "PS", "ps_5_0", NULL, 0, &pCompilePS, NULL);
			pDevice_->CreatePixelShader(pCompilePS->GetBufferPointer(), pCompilePS->GetBufferSize(), NULL, &shaderBundle[SHADER_BILLBOARD].pPixelShader);


			// 頂点レイアウトの作成（1頂点の情報が何のデータをどんな順番で持っているか）
			D3D11_INPUT_ELEMENT_DESC layout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, vectorSize * 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, vectorSize * 1, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			};
			pDevice_->CreateInputLayout(layout, 2, pCompileVS->GetBufferPointer(), pCompileVS->GetBufferSize(), &shaderBundle[SHADER_BILLBOARD].pVertexLayout);


			//シェーダーが無事作成できたので、コンパイルしたやつはいらない
			pCompileVS->Release();
			pCompilePS->Release();

			//ラスタライザ作成
			D3D11_RASTERIZER_DESC rdc = {};
			rdc.CullMode = D3D11_CULL_NONE;
			rdc.FillMode = D3D11_FILL_SOLID;
			rdc.FrontCounterClockwise = TRUE;
			pDevice_->CreateRasterizerState(&rdc, &shaderBundle[SHADER_BILLBOARD].pRasterizerState);
		}

	}


	//今から描画するShaderBundleを設定
	void SetShader(SHADER_TYPE type)
	{
		pContext_->RSSetState(shaderBundle[type].pRasterizerState);
		pContext_->VSSetShader(shaderBundle[type].pVertexShader, NULL, 0);                         // 頂点シェーダをセット
		pContext_->PSSetShader(shaderBundle[type].pPixelShader, NULL, 0);                          // ピクセルシェーダをセット
		pContext_->IASetInputLayout(shaderBundle[type].pVertexLayout);
	}

	//ブレンドモードの変更
	void SetBlendMode(BLEND_MODE blendMode)
	{
		//加算合成
		float blendFactor[4] = { D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO };
		pContext_->OMSetBlendState(pBlendState[blendMode], blendFactor, 0xffffffff);
		
		//Zバッファへの書き込み
		pContext_->OMSetDepthStencilState(pDepthStencilState[blendMode], 0);
	}

	//描画開始
	void BeginDraw()
	{
		//何か準備できてないものがあったら諦める
		if (NULL == pDevice_) return;
		if (NULL == pContext_) return;
		if (NULL == pRenderTargetView_) return;
		if (NULL == pSwapChain_) return;
		pContext_->OMSetRenderTargets(1, &pRenderTargetView_, pDepthStencilView);

		//背景の色
		float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };//背景は透明にしておくね

		//画面をクリア
		pContext_->ClearRenderTargetView(pRenderTargetView_, clearColor);

		//深度バッファクリア
		pContext_->ClearDepthStencilView(pDepthStencilView, D3D11_CLEAR_DEPTH, 1.0f, 0);	
	}


	//描画終了
	void EndDraw()
	{
		//スワップ（バックバッファを表に表示する）
		pSwapChain_->Present(1, 0);
	}


	//開放処理
	void Release()
	{
		SAFE_RELEASE(pDepthStencil);
		SAFE_RELEASE(pDepthStencilView);
		SAFE_RELEASE(pRenderTargetView_);
		SAFE_RELEASE(pSwapChain_);

		for (int i = 0; i < BLEND_MAX; i++)
		{
			SAFE_RELEASE(pBlendState[i]);
			SAFE_RELEASE(pDepthStencilState[i]);
		}

		for (int i = 0; i < SHADER_MAX; i++)
		{
			SAFE_RELEASE(shaderBundle[i].pRasterizerState);
			SAFE_RELEASE(shaderBundle[i].pVertexLayout);
			SAFE_RELEASE(shaderBundle[i].pVertexShader);
			SAFE_RELEASE(shaderBundle[i].pPixelShader);
		}

		if (pContext_)
		{
			pContext_->ClearState();
		}

		SAFE_RELEASE(pCompositionVisual_);
		SAFE_RELEASE(pCompositionTarget_);
		SAFE_RELEASE(pCompositionDevice_);
		SAFE_RELEASE(pContext_);
		SAFE_RELEASE(pDevice_);
	}


	//三角形と線分の衝突判定（衝突判定に使用）
	//https://pheema.hatenablog.jp/entry/ray-triangle-intersection
	bool Intersect(XMFLOAT3 & start, XMFLOAT3 & direction, XMFLOAT3 & v0, XMFLOAT3 & v1, XMFLOAT3 & v2, float* distance)
	{
		// 微小な定数([M?ller97] での値)
		constexpr float kEpsilon = 1e-6f;

		//三角形の２辺
		XMVECTOR edge1 = XMVectorSet(v1.x - v0.x, v1.y - v0.y, v1.z - v0.z, 0.0f);
		XMVECTOR edge2 = XMVectorSet(v2.x - v0.x, v2.y - v0.y, v2.z - v0.z, 0.0f);

		XMVECTOR alpha = XMVector3Cross(XMLoadFloat3(&direction), edge2);
		float det = XMVector3Dot(edge1, alpha).m128_f32[0];

		// 三角形に対して、レイが平行に入射するような場合 det = 0 となる。
		// det が小さすぎると 1/det が大きくなりすぎて数値的に不安定になるので
		// det ? 0 の場合は交差しないこととする。
		if (-kEpsilon < det && det < kEpsilon) 
		{
			return false;
		}

		float invDet = 1.0f / det;
		XMFLOAT3 r = XMFLOAT3(start.x - v0.x, start.y - v0.y, start.z - v0.z);

		// u が 0 <= u <= 1 を満たしているかを調べる。
		float u = XMVector3Dot(alpha, XMLoadFloat3(&r)).m128_f32[0] * invDet;
		if (u < 0.0f || u > 1.0f)
		{
			return false;
		}

		XMVECTOR beta = XMVector3Cross(XMLoadFloat3(&r), edge1);

		// v が 0 <= v <= 1 かつ u + v <= 1 を満たすことを調べる。
		// すなわち、v が 0 <= v <= 1 - u をみたしているかを調べればOK。
		float v = XMVector3Dot(XMLoadFloat3(&direction), beta).m128_f32[0] * invDet;
		if (v < 0.0f || u + v > 1.0f) 
		{
			return false;
		}

		// t が 0 <= t を満たすことを調べる。
		float t = XMVector3Dot(edge2, beta).m128_f32[0] * invDet;
		if (t < 0.0f) 
		{
			return false;
		}

		*distance = t;
		return true;
	}

	//Zバッファへの書き込みON/OFF
	void SetDepthBafferWriteEnable(bool isWrite)
	{
		//ON
		if (isWrite)
		{
			//Zバッファ（デプスステンシルを指定する）
			pContext_->OMSetRenderTargets(1, &pRenderTargetView_, pDepthStencilView);
		}

		//OFF
		else
		{
			pContext_->OMSetRenderTargets(1, &pRenderTargetView_, nullptr);
		}
	}

}
