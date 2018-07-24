  /*-----------------------.
  | :: Greeting message :: |
  '-----------------------*/

#pragma message "\
SweetFX 2.0 by CeeJay.dk\n\
\n"

#if __RENDERER__ == 0x061
  #pragma message "Rendering using OpenGL\n"

#elif __RENDERER__ == 0xD3D9
  #pragma message "Rendering using D3D9\n"
  
#elif __RENDERER__ == 0xD3D10
  #pragma message "Rendering using D3D10\n"
  
#elif __RENDERER__ == 0xD3D11
  #pragma message "Rendering using D3D11\n"
  
#endif

#pragma message "\n\
Press Printscreen to take screenshots\n"

#if ReShade_ToggleKey == VK_SCROLL
  #pragma message "Press Scrolllock to toggle effects on/off\n"
#else
	#pragma message "Press your custom toggle key (see the Reshade settings) to toggle effects on/off\n"
#endif

#pragma message "\nReticulating splines.."