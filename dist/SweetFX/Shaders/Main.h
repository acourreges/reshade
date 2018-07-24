  /*------------------------------.
  | :: Include enabled shaders :: |
  '------------------------------*/

/* //didn't make it into 1.6
#if (USE_PRINT == 1) //I could make this always on
  #include "SweetFX\Shaders\Print.h"
#endif
*/

//didn't make it into 1.6	
/*
#if (USE_SWEETCRT == 1)
  #include "SweetFX\Shaders\SweetCRT.h"
  #define Need_sRGB 1
#endif
*/

#if (USE_LEVELS == 1)
  #include "SweetFX\Shaders\Levels.h"
  #define Need_sRGB 1
#endif

#if (USE_TECHNICOLOR == 1)
  #include "SweetFX\Shaders\Technicolor.h"
  #define Need_sRGB 1
#endif

#if (USE_DPX == 1)
  #include "SweetFX\Shaders\DPX.h"
  #define Need_sRGB 1
#endif

#if (USE_MONOCHROME == 1)
  #include "SweetFX\Shaders\Monochrome.h"
  #define Need_sRGB 1
#endif

#if (USE_COLORMATRIX == 1)
  #include "SweetFX\Shaders\ColorMatrix.h"
  #define Need_sRGB 1
#endif

#if (USE_LIFTGAMMAGAIN == 1)
  #include "SweetFX\Shaders\LiftGammaGain.h"
  #define Need_sRGB 1
#endif

#if (USE_TONEMAP == 1)
  #include "SweetFX\Shaders\Tonemap.h"
  #define Need_sRGB 1
#endif

#if (USE_VIBRANCE == 1)
  #include "SweetFX\Shaders\Vibrance.h"
  #define Need_sRGB 1
#endif

#if (USE_CURVES == 1)
  #include "SweetFX\Shaders\Curves.h"
  #define Need_sRGB 1
#endif

#if (USE_SEPIA == 1)
  #include "SweetFX\Shaders\Sepia.h"
  #define Need_sRGB 1
#endif

/* //didn't make it into 1.6	
#if (USE_DALTONIZE == 1)
  #include "SweetFX\Shaders\Daltonize.h"
  #define Need_sRGB 1
#endif
*/

#if (USE_VIGNETTE == 1)
  #include "SweetFX\Shaders\Vignette.h"
  #define Need_sRGB 1
#endif

#if (USE_FILMGRAIN == 1)
  #include "SweetFX\Shaders\FilmGrain.h"
  #define Need_sRGB 1
#endif

#if (USE_DITHER == 1)
  #include "SweetFX\Shaders\Dither.h"
  #define Need_sRGB 1
#endif

#if (USE_BORDER == 1)
  #include "SweetFX\Shaders\Border.h"
  #define Need_sRGB 1
#endif

#if (USE_SPLITSCREEN == 1)
  #include "SweetFX\Shaders\Splitscreen.h"
  #define Need_sRGB 1
#endif

  /*--------------------.
  | :: Effect passes :: |
  '--------------------*/

//float4 SweetFX_main(float2 tex, float4 FinalColor) //RadeonPro expects main()
float4 main(float2 tex, float4 FinalColor)
{

/*
  // SweetCRT
  #if (USE_SWEETCRT == 1)
    FinalColor = SweetCRTPass(FinalColor,tex);
  #endif
*/

  // Levels
  #if (USE_LEVELS == 1)
	FinalColor = LevelsPass(FinalColor);
  #endif

  // Technicolor
  #if (USE_TECHNICOLOR == 1)
	FinalColor = TechnicolorPass(FinalColor);
  #endif

  // DPX
  #if (USE_DPX == 1)
	FinalColor = DPXPass(FinalColor);
  #endif

  // Monochrome
  #if (USE_MONOCHROME == 1)
	FinalColor = MonochromePass(FinalColor);
  #endif

  // ColorMatrix
  #if (USE_COLORMATRIX == 1)
	FinalColor = ColorMatrixPass(FinalColor);
  #endif

  // Lift Gamma Gain
  #if (USE_LIFTGAMMAGAIN == 1)
	FinalColor = LiftGammaGainPass(FinalColor);
  #endif

  // Tonemap
  #if (USE_TONEMAP == 1)
	FinalColor = TonemapPass(FinalColor);
  #endif

  // Vibrance
  #if (USE_VIBRANCE == 1)
	FinalColor = VibrancePass(FinalColor);
  #endif

  // Curves
  #if (USE_CURVES == 1)
	FinalColor = CurvesPass(FinalColor);
  #endif

  // Sepia
  #if (USE_SEPIA == 1)
    FinalColor = SepiaPass(FinalColor);
  #endif

  // Vignette
  #if (USE_VIGNETTE == 1)
	FinalColor = VignettePass(FinalColor,tex);
  #endif
 
/* //didn't make it into 1.6
  // Daltonize
  #if (USE_DALTONIZE == 1)
    FinalColor = Daltonize(FinalColor);
  #endif
*/

  // FilmGrain
  #if (USE_FILMGRAIN == 1)
	FinalColor = FilmGrainPass(FinalColor,tex);
  #endif
  
  // Dither (should go near the end as it only dithers what went before it)
  #if (USE_DITHER == 1)
	FinalColor = DitherPass(FinalColor,tex);
  #endif

  // Border
  #if (USE_BORDER == 1)
    FinalColor = BorderPass(FinalColor,tex);
  #endif

  // Splitscreen
  #if (USE_SPLITSCREEN == 1)
	  FinalColor = SplitscreenPass(FinalColor,tex);
  #endif

/* //didn't make it into 1.6
  #if (USE_PRINT == 1)
    FinalColor = PrintPass(FinalColor,tex);
  #endif
*/  

  // Return FinalColor
  return FinalColor;
}
