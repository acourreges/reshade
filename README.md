ReShade - Metal Gear Solid V Fork
===

This is the fork of [ReShade](https://reshade.me/) which was used to write the [Metal Gear Solid V - Graphics Study](http://www.adriancourreges.com/blog/2017/12/15/mgs-v-graphics-study/) article.

The code is branched from the original ReShade 0.10.0 version which has a hooking mechanism compatible with the game.
It was extended with more hooks and some logic to capture and dump render targets, extract shader bytecode, toggle some draw calls... 
It's all very rough around the edges, not much time spent on cleaning it up but here it is if you want to play with it. 

## Install

- Setup your game to render in 1280x720 in windowed mode and exit the game
- Download the release package (`dist` folder in the repository or [GitHub release](https://github.com/acourreges/reshade/releases/)) and extract it into your game folder which should contain the game exe `mgsvtpp.exe`.  
You will end up with `dxgi.dll`, `Sweet.fx` and `SweetFX` next to your original `mgsvtpp.exe`
- Run the game, you should see ReShade statistics displayed in the corner of the window meaning the hooks are in place.

## Features

- **Display G-Buffer or other render targets**  
Iterate over the render target list by pressing `F12` several times.
- **Dump intermediate content of render targets mid-rendering**  
Press `PrintScreen`, a new folder will appear next to `mgsvtpp.exe` containing PNG dumps.  
See `d3d11.cpp` for more granular control of the dump frequency (`MetaCL::OnDraw()` and `POOL_720P_COUNT`)
- **Dump shader source code**  
Set to true the variables you wish inside `d3d11.cpp` (like `DumpShaderPS`) and recompile the DLL. The game will now output DXBC raw data in a folder next to `mgsvtpp.exe`, containing clear text HLSL sources. 
- **Toggle Ishmael's bandage**  
Press `F9`.

## Build

Simply open `ReShade.sln` with Visual Studio 2015 and compile the `Reshade` project for the configuration `Release - x64`.  
To make things simple I included all the required dependencies like `boost` compiled for Win64 so you don't have to fetch and build anything else yourself.  
The compiled ReShade library can be found inside `bin\x64\Release\ReShade64.dll`, just rename it to `dxgi.dll` and copy it to your game folder. 

## Technical Notes

You can specify another resolution than 1280x720, see in `d3d11.cpp` the defines `BACK_BUFFER_WIDTH` and `BACK_BUFFER_HEIGHT`. 
Ideally these values should be deduced automatically from the swap chain configuration but the game resizes several time its swap-chain,
the last resize happening post render target creations making it harder to know which buffer to track beforehand. 
I didn't get around to automate this part. 

The game performs all its rendering using D3D11 command lists in deferred contexts, complicating the tracking of states. 
For example `OMGetRenderTargets()` won't return the list of render targets currently bound so it requires separate tracking 
for this.  

## License

All the source code is licensed under the conditions of the BSD 3-clause license.


