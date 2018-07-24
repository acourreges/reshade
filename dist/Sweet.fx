
  /*---------------------------------------.
  |   .-.                   .  .---..   .  |
  |  (   )                 _|_ |     \ /   |
  |   `-..  .    ._.-.  .-. |  |---   /    |
  |  (   )\  \  / (.-' (.-' |  |     / \   |
  |   `-'  `' `'   `--' `--'`-''    '   '  |
  |              by CeeJay.dk              | 
  '---------------------------------------*/
  /*---------------------------------------.
  | ::     Using ReShade by Crosire     :: | 
  '---------------------------------------*/

  /*-----------------------.
  | ::     Settings     :: |
  '-----------------------*/

#include "SweetFX/Keycodes.txt" //load keycode aliases for the Reshade key bindings
#include "SweetFX/ReShade_settings.txt" //load ReShade settings
#include "SweetFX/SweetFX_settings.txt" //load SweetFX settings

  /*-----------------------.
  | :: Greeting message :: |
  '-----------------------*/

#include "SweetFX/Shaders/Greeting.h"

#if ReShade_ShowStatistics != 0
	#pragma reshade statistics
#endif

  /*-----------------------.
  | ::      Globals     :: |
  '-----------------------*/

#include "SweetFX/Shaders/Globals.h" //define global contants and uniforms

  /*-----------------------.
  | ::     Textures     :: |
  '-----------------------*/
  
/*
Default values if explicitly not set are:
	Width = 1;
	Height = 1;
	Format = R8G8B8A8;
	MipLevels = 1;
*/

texture colorTex : SV_Target; //Game texture
texture depthTex : SV_Depth;  //Game depth

#if USE_TRANSITION == 1
  texture transitionTex < string source = "SweetFX/Textures/" Transition_texture ; >
  {
    Width = Transition_texture_width;
    Height = Transition_texture_height;
  };
#endif 


#if (USE_SMAA == 1 || USE_SMAA_ANTIALIASING == 1)
  texture edgesTex
  {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = R8G8B8A8;   
  };

  texture blendTex
  {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = R8G8B8A8;
  };

  texture areaTex < string source = "SweetFX/Textures/SMAA_AreaTex.dds"; >
  {
    Width = 160;
    Height = 560;
    Format = R8G8;
  };
  
  texture searchTex < string source = "SweetFX/Textures/SMAA_SearchTex.dds"; >
  {
    Width = 64;
    Height = 16;
    Format = R8;
  };
#endif 

  /*-----------------------.
  | ::     Samplers     :: |
  '-----------------------*/

sampler colorGammaSampler
{
	Texture = colorTex;
  AddressU  = Clamp; AddressV = Clamp;
  MipFilter = Linear; MinFilter = Linear; MagFilter = Linear; //Why Mipfilter linear - shouldn't point be fine?
  SRGBTexture = false;
};

sampler colorLinearSampler
{
	Texture = colorTex;
	AddressU  = Clamp; AddressV = Clamp;
	MipFilter = Point; MinFilter = Linear; MagFilter = Linear;
	SRGBTexture = true;
};

#if USE_TRANSITION == 1
  sampler transitionSampler
  {
    Texture = transitionTex;
  };
#endif

#if (USE_SMAA == 1 || USE_SMAA_ANTIALIASING == 1)

	sampler edgesSampler
	{
		Texture = edgesTex;
		AddressU = Clamp; AddressV = Clamp;
		MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
		SRGBTexture = false;
	};

	sampler blendSampler
	{
		Texture = blendTex;
		AddressU = Clamp; AddressV = Clamp;
		MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
		SRGBTexture = false;
	};

	sampler areaSampler
	{
		Texture = areaTex;
		AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
		MipFilter = Linear; MinFilter = Linear; MagFilter = Linear;
		SRGBTexture = false;
	};

  sampler searchSampler
  {
    Texture = searchTex;
    AddressU = Clamp; AddressV = Clamp; AddressW = Clamp;
    MipFilter = Point; MinFilter = Point; MagFilter = Point;
    SRGBTexture = false;
  };

#endif

sampler depthSampler
{
	Texture = depthTex;
	AddressU  = Clamp; AddressV = Clamp;
	MipFilter = Linear; MinFilter = Linear; MagFilter = Linear; //Why Mipfilter linear - shouldn't point be fine?
	// SRGBTexture = true; // The depth buffer is always linear and cannot be set to gamma.
};

#define predicationSampler depthSampler //Use the depth sampler as our predication sampler

  /*-----------------------.
  | ::     Effects      :: |
  '-----------------------*/
 
#define s0 colorGammaSampler
#define s1 colorLinearSampler
#define myTex2D tex2D 

#include "SweetFX/Shaders/Main.h"

  /*----------------------.
  | ::  Vertex Shader  :: |
  '----------------------*/

void FullscreenTriangle(in uint id : SV_VertexID, out float4 position : SV_Position, out float2 texcoord : TEXCOORD0)
{
	// Basic Buffer/Layout-less fullscreen triangle vertex shader - used for several of the passes.
	texcoord.x = (id == 2) ? 2.0 : 0.0;
	texcoord.y = (id == 1) ? 2.0 : 0.0;
	position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}



  /*-----------------------.
  | ::     Cartoon      :: |
  '-----------------------*/

#if (USE_CARTOON == 1)
  // Cartoon
  #include "SweetFX\Shaders\Cartoon.h"
  #include "SweetFX\Shaders\CartoonWrap.h"
#endif

  /*-----------------------.
  | ::   LumaSharpen    :: |
  '-----------------------*/
  
#if (USE_LUMASHARPEN == 1)
  // LumaSharpen
  #include "SweetFX\Shaders\LumaSharpen.h"
  #include "SweetFX\Shaders\LumaSharpenWrap.h"
#endif

  /*--------------------.
  | ::     SMAA      :: |
  '--------------------*/

#if (USE_SMAA == 1 || USE_SMAA_ANTIALIASING == 1)

	#define SMAA_RT_METRICS float4(pixel, screen_size) //let SMAA know the size of a pixel and the screen
  
  #define SMAA_HLSL_3 1
  #define SMAA_PIXEL_SIZE pixel
  #define SMAA_PRESET_CUSTOM 1

  #include "SweetFX/Shaders/SMAA.hlsl"
  
	////////////////////////////
	// Vertex shader wrappers //
	////////////////////////////
	
	void SMAAEdgeDetectionVSWrap( in uint id : SV_VertexID,
																out float4 position : SV_Position,
																out float2 texcoord : TEXCOORD0,
																out float4 offset0 : TEXCOORD1,
																out float4 offset1 : TEXCOORD2,
																out float4 offset2 : TEXCOORD3 )
	{
		float4 offset[3];
		FullscreenTriangle(id, position, texcoord);
		SMAAEdgeDetectionVS(texcoord, offset);
		
		// Get around OpenGL not accepting array input/outputs
		offset0 = offset[0], offset1 = offset[1], offset2 = offset[2];
	}
	
	void SMAABlendingWeightCalculationVSWrap( in uint id : SV_VertexID,
																						out float4 position : SV_Position,
																						out float2 texcoord : TEXCOORD0,
																						out float2 pixcoord : TEXCOORD1,
																						out float4 offset0 : TEXCOORD2,
																						out float4 offset1 : TEXCOORD3,
																						out float4 offset2 : TEXCOORD4 )
	{
		float4 offset[3];
		FullscreenTriangle(id, position, texcoord);
		SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
		
		// Get around OpenGL not accepting array input/outputs
		offset0 = offset[0], offset1 = offset[1], offset2 = offset[2];
	}

	void SMAANeighborhoodBlendingVSWrap(in uint id : SV_VertexID,
																			out float4 position : SV_Position,
																			out float2 texcoord : TEXCOORD0,
																			out float4 offset : TEXCOORD1)
	{
		FullscreenTriangle(id, position, texcoord);
		SMAANeighborhoodBlendingVS(texcoord, offset);
	}

	///////////////////////////
	// Pixel shader wrappers //
	///////////////////////////

	float2 SMAALumaEdgeDetectionPSWrap(	float4 position : SV_Position,
																			float2 texcoord : TEXCOORD0,
																			float4 offset0 : TEXCOORD1,
																			float4 offset1 : TEXCOORD2,
																			float4 offset2 : TEXCOORD3) : SV_Target
	{
	
		float4 offset[3] = { offset0, offset1, offset2 };

		return SMAALumaEdgeDetectionPS(
																		texcoord,
																		offset,
																		colorGammaSampler
																		
																		#if SMAA_PREDICATION == 1
																		,predicationSampler
																		#endif
																	);
	}
	
	float2 SMAAColorEdgeDetectionPSWrap(float4 position : SV_Position,
																			float2 texcoord : TEXCOORD0,
																			float4 offset0 : TEXCOORD1,
																			float4 offset1 : TEXCOORD2,
																			float4 offset2 : TEXCOORD3) : SV_Target
	{
		float4 offset[3] = { offset0, offset1, offset2 };
		return SMAAColorEdgeDetectionPS(
																		texcoord,
																		offset,
																		colorGammaSampler
																		
																		#if SMAA_PREDICATION == 1
																		 ,predicationSampler
																		#endif
																	 );
	}
	
	float2 SMAADepthEdgeDetectionPSWrap(float4 position : SV_Position,
																			float2 texcoord : TEXCOORD0,
																			float4 offset0 : TEXCOORD1,
																			float4 offset1 : TEXCOORD2,
																			float4 offset2 : TEXCOORD3) : SV_Target
	{
		float4 offset[3] = { offset0, offset1, offset2 };
		return SMAADepthEdgeDetectionPS(texcoord, offset, depthSampler);
	}

	float4 SMAABlendingWeightCalculationPSWrap(	float4 position : SV_Position,
																							float2 texcoord : TEXCOORD0,
																							float2 pixcoord : TEXCOORD1,
																							float4 offset0 : TEXCOORD2,
																							float4 offset1 : TEXCOORD3,
																							float4 offset2 : TEXCOORD4) : SV_Target
	{
		float4 offset[3] = { offset0, offset1, offset2 };
		return SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edgesSampler, areaSampler, searchSampler, 0.0);
	}

	float3 SMAANeighborhoodBlendingPSWrap(float4 position : SV_Position,
																				float2 texcoord : TEXCOORD0,
																				float4 offset : TEXCOORD1) : SV_Target
	{
    #if SMAA_DEBUG_OUTPUT == 1
      return tex2D(edgesSampler, texcoord); // Show edgesTex
    #elif SMAA_DEBUG_OUTPUT == 2
      return tex2D(blendSampler, texcoord); // Show blendTex
    #elif SMAA_DEBUG_OUTPUT == 3
      return tex2D(areaSampler, texcoord); // Show areaTex
    #elif SMAA_DEBUG_OUTPUT == 4
      return tex2D(searchSampler, texcoord); // Show searchTex
    #elif SMAA_DEBUG_OUTPUT == 5
      return float3(1.0, 0.0, 0.0); // Show the stencil in red.
    #else
			float3 color = SMAANeighborhoodBlendingPS(texcoord, offset, colorLinearSampler, blendSampler).rgb;
			
			#if __RENDERER__ == 0x061 && __VENDOR__ == 0x10DE //if OpenGL and Nvidia
					color.rgb = color.bgr;
			#endif
      
      return color;
    #endif
	}

#endif

  /*--------------------.
  | ::     FXAA      :: |
  '--------------------*/

#if (USE_FXAA == 1 || USE_FXAA_ANTIALIASING == 1)

  #define FXAA_PC 1
  #define FXAA_HLSL_3 1
  #define FXAA_GREEN_AS_LUMA 1 //It's better to calculate luma in the previous pass and pass it, than to use this option.

  #include "SweetFX/Shaders/Fxaa3_11.h"

  float4 FXAA(in float4 position : SV_Position, in float2 texcoord : TEXCOORD0) : SV_Target
  {
    return FxaaPixelShader(texcoord, colorGammaSampler, pixel, float4(0.0f, 0.0f, 0.0f, 0.0f), fxaa_Subpix, fxaa_EdgeThreshold, fxaa_EdgeThresholdMin);
  }

#endif

  /*-----------------------.
  | ::      Bloom       :: |
  '-----------------------*/

#if (USE_BLOOM == 1)
  #include "SweetFX\Shaders\Bloom.h"
  #include "SweetFX\Shaders\BloomWrap.h"
#endif


  /*-----------------------.
  | ::       HDR        :: |
  '-----------------------*/

#if (USE_HDR == 1)
  #include "SweetFX\Shaders\HDR.h"
  #include "SweetFX\Shaders\HDRWrap.h"
#endif


  /*---------------------------.
  | :: Chromatic Aberration :: |
  '---------------------------*/

#if (USE_CA == 1)
  #include "SweetFX\Shaders\ChromaticAberration.h"
  #include "SweetFX\Shaders\ChromaticAberrationWrap.h"
#endif

  /*-----------------------.
  | ::     Explosion    :: |
  '-----------------------*/

#if (USE_EXPLOSION == 1)
  // Explosion
  #include "SweetFX\Shaders\Explosion.h"
  #include "SweetFX\Shaders\ExplosionWrap.h"
#endif

  /*-----------------------.
  | ::   Advanced CRT   :: |
  '-----------------------*/

#if (USE_ADVANCED_CRT == 1)
  // Advanced CRT
  #include "SweetFX\Shaders\AdvancedCRT.h"
  #include "SweetFX\Shaders\AdvancedCRTWrap.h"
#endif

  /*-----------------------.
  | ::   PixelArt CRT   :: |
  '-----------------------*/

#if (USE_PIXELART_CRT == 1)
  #include "SweetFX\Shaders\PixelArtCRT.h"
  #include "SweetFX\Shaders\PixelArtCRTWrap.h"
#endif

  /*-----------------------.
  | ::      Shared      :: |
  '-----------------------*/

void SharedWrap(in float4 position : SV_Position, in float2 texcoord : TEXCOORD0, out float3 color : SV_Target)
{
	color = tex2D(colorGammaSampler, texcoord).rgb;
	
  //#if __RENDERER__ != 0x061 //if not OpenGL
    //Depth buffer access
    
    float depth = tex2D(depthSampler, texcoord).x;
    
    if (depthtoggle)
    {
      // Linearize depth
      const float z_near = 1.0;   // camera z near
      const float z_far  = 100.0; // camera z far
      depth = (2.0 * z_near) / ( -(z_far - z_near) * depth + (z_far + z_near) );	
    
      color.rgb = float3(depth.xxx);
    }
  //#endif
  
	color = main(texcoord, color.rgbb).rgb; // Add effects. Note : RadeonPro uses main() as the entry point. Renaming it breaks RadeonPro compatibility.
	//color = SweetFX_main(texcoord, color); // Add effects
}


  /*-----------------------.
  | ::      Custom      :: |
  '-----------------------*/
#if (USE_CUSTOM == 1)
  #include "SweetFX\Shaders\Custom.h"
#endif

  /*----------------------.
  | ::   Transitions   :: |
  '----------------------*/

#if USE_TRANSITION == 1
  #include "SweetFX/Shaders/Transitions.h"
#endif

  /*-----------------------.
  | ::    Techniques    :: |
  '-----------------------*/

technique Default < bool enabled = true; int toggle = ReShade_ToggleKey; >
{
#if (USE_CARTOON == 1)
	pass //Cartoon pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = CartoonWrap;
	}
#endif

#if (USE_LUMASHARPEN == 1)
	pass //LumaSharpen pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = LumaSharpenWrap;
	}
#endif

#if (USE_SMAA == 1 || USE_SMAA_ANTIALIASING == 1)

  // SMAA //
	pass SMAA_EdgeDetection //First SMAA Pass
	{
		VertexShader = SMAAEdgeDetectionVSWrap;

	#if SMAA_EDGE_DETECTION == 1
		PixelShader = SMAALumaEdgeDetectionPSWrap;
	#elif SMAA_EDGE_DETECTION == 3
		PixelShader = SMAADepthEdgeDetectionPSWrap;
	#else
		PixelShader = SMAAColorEdgeDetectionPSWrap; //Probably the best in most cases so I default to this.
	#endif

		// We will be creating the stencil buffer for later usage.
		StencilEnable = true;
		StencilPass = REPLACE;
		StencilRef = 1;
		
		RenderTarget = edgesTex;
	}
	
	pass SMAA_BlendWeightCalculation //Second SMAA Pass
	{
		VertexShader = SMAABlendingWeightCalculationVSWrap;
		PixelShader = SMAABlendingWeightCalculationPSWrap;
        
		// Here we want to process only marked pixels.
		StencilEnable = true;
		StencilPass = KEEP;
		StencilFunc = EQUAL;
		StencilRef = 1;
		
		RenderTarget = blendTex;
	}
	
	pass SMAA_NeighborhoodBlending //Third SMAA Pass
	{
		VertexShader = SMAANeighborhoodBlendingVSWrap;
		PixelShader  = SMAANeighborhoodBlendingPSWrap;
		
	#if SMAA_DEBUG_OUTPUT == 5
		// Use the stencil so we can show it.
		StencilEnable = true;
		StencilPass = KEEP;
		StencilFunc = EQUAL;
		StencilRef = 1;
	#else
		// Process all the pixels.
		StencilEnable = false;
	#endif
	
		SRGBWriteEnable = true;
	}
#endif

#if (USE_EXPLOSION == 1)
	pass //Explosion pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = ExplosionWrap;
	}
#endif

#if (USE_FXAA == 1 || USE_FXAA_ANTIALIASING == 1)
  //TODO make a luma pass
	pass FXAA
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = FXAA;
	}
#endif

#if (USE_BLOOM == 1)
	pass //Bloom pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = BloomWrap;
	}
#endif

#if (USE_HDR == 1)
	pass //HDR pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = HDRWrap;
	}
#endif

#if (USE_CA == 1)
	pass //CA pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = ChromaticAberrationWrap;
	}
#endif

#if (USE_ADVANCED_CRT == 1)
	pass //Advanced CRT pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = AdvancedCRTWrap;
	}
#endif

#if (USE_PIXELART_CRT == 1)
	pass //Pixel Art CRT pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = PixelArtCRTWrap;
		
		SRGBWriteEnable = true; //PixelArtCRT uses linear so we must convert to gamma again
	}
#endif


	pass //Shared pass - the effects that don't require a seperate pass are all done in this one.
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = SharedWrap;
	}
	
#if (USE_CUSTOM == 1) //There is no way of knowing where the users own shader should go so let us just put it at the end for now
	pass //Custom pass
	{
		VertexShader = FullscreenTriangle;
		PixelShader  = CustomPass;
	}
#endif


}

  /*---------------------------.
  | :: Temporary techniques :: |
  '---------------------------*/

#if USE_TRANSITION == 1
  technique Welcome < bool enabled = true; int timeout = Transition_time; > //sets the timeleft value. When it reaches 0 the technique is disabled.
  {
    pass
    {
      VertexShader = FullscreenTriangle;
      PixelShader  = Transition_type;
    }
  }
#endif
