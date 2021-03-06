
#include <windows.h>
#include "resource.h"

#include "sysconfig.h"
#include "sysdeps.h"

#if defined (D3D) && defined (GFXFILTER)

#define D3DXFX_LARGEADDRESS_HANDLE
#ifdef D3DXFX_LARGEADDRESS_HANDLE
#define EFFECTCOMPILERFLAGS D3DXFX_LARGEADDRESSAWARE
#else
#define EFFECTCOMPILERFLAGS 0
#endif

#define EFFECT_VERSION 3
#define D3DX9DLL _T("d3dx9_43.dll")
#define TWOPASS 1
#define SHADER 1

#include "options.h"
#include "xwin.h"
#include "custom.h"
#include "drawing.h"
#include "dxwrap.h"
#include "win32.h"
#include "win32gfx.h"
#include "gfxfilter.h"
#include "statusline.h"
#include "hq2x_d3d.h"
#include "zfile.h"
#include "uae.h"

extern int D3DEX, d3ddebug;
int forcedframelatency = -1;

#include <d3d9.h>
#include <d3dx9.h>

#include "direct3d.h"

static TCHAR *D3DHEAD = _T("-");
static int psEnabled, psActive, psPreProcess, shaderon;

static bool showoverlay = true;

#define MAX_PASSES 2

static D3DFORMAT tformat;
static int d3d_enabled, d3d_ex;
static IDirect3D9 *d3d;
static IDirect3D9Ex *d3dex;
static IDirect3DSwapChain9 *d3dswapchain;
static D3DPRESENT_PARAMETERS dpp;
static D3DDISPLAYMODEEX modeex;
static IDirect3DDevice9 *d3ddev;
static IDirect3DDevice9Ex *d3ddevex;
static D3DSURFACE_DESC dsdbb;
static LPDIRECT3DTEXTURE9 texture, sltexture, ledtexture, masktexture, mask2texture, blanktexture;
static IDirect3DQuery9 *query;
static int masktexture_w, masktexture_h;
static float mask2texture_w, mask2texture_h, mask2texture_ww, mask2texture_wh;
static float mask2texture_wwx, mask2texture_hhx, mask2texture_minusx, mask2texture_minusy;
static float mask2texture_multx, mask2texture_multy, mask2texture_offsetw;
static LPDIRECT3DTEXTURE9 lpWorkTexture1[2], lpWorkTexture2[2], lpTempTexture;
LPDIRECT3DTEXTURE9 cursorsurfaced3d;
static LPDIRECT3DVOLUMETEXTURE9 lpHq2xLookupTexture;
static IDirect3DVertexBuffer9 *vertexBuffer;
static ID3DXSprite *sprite;
static HWND d3dhwnd;
static int devicelost;
static int locked, fulllocked;
static int cursor_offset_x, cursor_offset_y, cursor_offset2_x, cursor_offset2_y;
static float maskmult_x, maskmult_y;
RECT mask2rect;
static bool wasstilldrawing_broken;
static bool renderdisabled;
static HANDLE filenotificationhandle;
static int extrapasses;

static bool fakemode;
static uae_u8 *fakebitmap;

static D3DXMATRIXA16 m_matProj, m_matProj2;
static D3DXMATRIXA16 m_matWorld, m_matWorld2;
static D3DXMATRIXA16 m_matView, m_matView2;
static D3DXMATRIXA16 m_matPreProj;
static D3DXMATRIXA16 m_matPreView;
static D3DXMATRIXA16 m_matPreWorld;
static D3DXMATRIXA16 postproj;
static D3DXVECTOR4 maskmult, maskshift, texelsize;
static D3DXVECTOR4 fakesize;

static int ledwidth, ledheight;
static int max_texture_w, max_texture_h;
static int tin_w, tin_h, tout_w, tout_h, window_h, window_w;
static int t_depth, mult, multx;
static int required_sl_texture_w, required_sl_texture_h;
static int vsync2, guimode, maxscanline;
static int resetcount;
static double cursor_x, cursor_y;
static bool cursor_v;

#define NUMVERTICES 8
#define D3DFVF_TLVERTEX D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1
struct TLVERTEX {
	D3DXVECTOR3 position;       // vertex position
	D3DCOLOR    diffuse;
	D3DXVECTOR2 texcoord;       // texture coords
};

static int ddraw_fs;
static int ddraw_fs_attempt;
static LPDIRECTDRAW7 ddraw;

static void ddraw_fs_hack_free (void)
{
	HRESULT hr;

	if (!ddraw_fs)
		return;
	if (ddraw_fs == 2)
		ddraw->RestoreDisplayMode ();
	hr = ddraw->SetCooperativeLevel (d3dhwnd, DDSCL_NORMAL);
	if (FAILED (hr)) {
		write_log (_T("IDirectDraw7_SetCooperativeLevel CLEAR: %s\n"), DXError (hr));
	}
	ddraw->Release ();
	ddraw = NULL;
	ddraw_fs = 0;

}

static int ddraw_fs_hack_init (void)
{
	HRESULT hr;
	struct MultiDisplay *md;

	ddraw_fs_hack_free ();
	DirectDraw_get_GUIDs ();
	md = getdisplay (&currprefs);
	if (!md)
		return 0;
	hr = DirectDrawCreateEx (md->primary ? NULL : &md->ddguid, (LPVOID*)&ddraw, IID_IDirectDraw7, NULL);
	if (FAILED (hr)) {
		write_log (_T("DirectDrawCreateEx failed, %s\n"), DXError (hr));
		return 0;
	}
	ddraw_fs = 1;
	hr = ddraw->SetCooperativeLevel (d3dhwnd, DDSCL_ALLOWREBOOT | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
	if (FAILED (hr)) {
		write_log (_T("IDirectDraw7_SetCooperativeLevel SET: %s\n"), DXError (hr));
		ddraw_fs_hack_free ();
		return 0;
	}
	hr = ddraw->SetDisplayMode (dpp.BackBufferWidth, dpp.BackBufferHeight, t_depth, dpp.FullScreen_RefreshRateInHz, 0);
	if (FAILED (hr)) {
		write_log (_T("1:IDirectDraw7_SetDisplayMode: %s\n"), DXError (hr));
		if (dpp.FullScreen_RefreshRateInHz && isvsync_chipset () < 0) {
			hr = ddraw->SetDisplayMode (dpp.BackBufferWidth, dpp.BackBufferHeight, t_depth, 0, 0);
			if (FAILED (hr))
				write_log (_T("2:IDirectDraw7_SetDisplayMode: %s\n"), DXError (hr));
		}
		if (FAILED (hr)) {
			write_log (_T("IDirectDraw7_SetDisplayMode: %s\n"), DXError (hr));
			ddraw_fs_hack_free ();
			return 0;
		}
	}
	ddraw_fs = 2;
	return 1;
}

static TCHAR *D3D_ErrorText (HRESULT error)
{
	return _T("");
}
static TCHAR *D3D_ErrorString (HRESULT dival)
{
	static TCHAR dierr[200];
	_stprintf (dierr, _T("%08X S=%d F=%04X C=%04X (%d) (%s)"),
		dival, (dival & 0x80000000) ? 1 : 0,
		HRESULT_FACILITY(dival),
		HRESULT_CODE(dival),
		HRESULT_CODE(dival),
		D3D_ErrorText (dival));
	return dierr;
}

static D3DXMATRIX* MatrixOrthoOffCenterLH (D3DXMATRIXA16 *pOut, float l, float r, float b, float t, float zn, float zf)
{
	pOut->_11=2.0f/r; pOut->_12=0.0f;   pOut->_13=0.0f;  pOut->_14=0.0f;
	pOut->_21=0.0f;   pOut->_22=2.0f/t; pOut->_23=0.0f;  pOut->_24=0.0f;
	pOut->_31=0.0f;   pOut->_32=0.0f;   pOut->_33=1.0f;  pOut->_34=0.0f;
	pOut->_41=-1.0f;  pOut->_42=-1.0f;  pOut->_43=0.0f;  pOut->_44=1.0f;
	return pOut;
}

static D3DXMATRIX* MatrixScaling (D3DXMATRIXA16 *pOut, float sx, float sy, float sz)
{
	pOut->_11=sx;     pOut->_12=0.0f;   pOut->_13=0.0f;  pOut->_14=0.0f;
	pOut->_21=0.0f;   pOut->_22=sy;     pOut->_23=0.0f;  pOut->_24=0.0f;
	pOut->_31=0.0f;   pOut->_32=0.0f;   pOut->_33=sz;    pOut->_34=0.0f;
	pOut->_41=0.0f;   pOut->_42=0.0f;   pOut->_43=0.0f;  pOut->_44=1.0f;
	return pOut;
}

static D3DXMATRIX* MatrixTranslation (D3DXMATRIXA16 *pOut, float tx, float ty, float tz)
{
	pOut->_11=1.0f;   pOut->_12=0.0f;   pOut->_13=0.0f;  pOut->_14=0.0f;
	pOut->_21=0.0f;   pOut->_22=1.0f;   pOut->_23=0.0f;  pOut->_24=0.0f;
	pOut->_31=0.0f;   pOut->_32=0.0f;   pOut->_33=1.0f;  pOut->_34=0.0f;
	pOut->_41=tx;     pOut->_42=ty;     pOut->_43=tz;    pOut->_44=1.0f;
	return pOut;
}

static TCHAR *D3DX_ErrorString (HRESULT hr, LPD3DXBUFFER Errors)
{
	static TCHAR buffer[1000];
	TCHAR *s = NULL;

	buffer[0] = 0;
	if (Errors)
		s = au ((char*)Errors->GetBufferPointer ());
	if (hr != S_OK)
		_tcscpy (buffer, D3D_ErrorString (hr));
	if (s) {
		if (buffer[0])
			_tcscat (buffer, _T(" "));
		_tcscat (buffer, s);
	}
	xfree (s);
	return buffer;
}

static int isd3d (void)
{
	if (fakemode || devicelost || !d3ddev || !d3d_enabled || renderdisabled)
		return 0;
	return 1;
}

static LPD3DXEFFECT postEffect;
static D3DXHANDLE postSourceTextureHandle;
static D3DXHANDLE postMaskTextureHandle;
static D3DXHANDLE postTechnique, postTechniquePlain, postTechniqueAlpha;
static D3DXHANDLE postMatrixSource;
static D3DXHANDLE postMaskMult, postMaskShift;
static D3DXHANDLE postFilterMode;
static D3DXHANDLE postTexelSize;

static LPD3DXEFFECT pEffect;
static D3DXEFFECT_DESC EffectDesc;
static float m_scale;
static LPCSTR m_strName;
// Matrix Handles
static D3DXHANDLE m_MatWorldEffectHandle;
static D3DXHANDLE m_MatViewEffectHandle;
static D3DXHANDLE m_MatProjEffectHandle;
static D3DXHANDLE m_MatWorldViewEffectHandle;
static D3DXHANDLE m_MatViewProjEffectHandle;
static D3DXHANDLE m_MatWorldViewProjEffectHandle;
// Texture Handles
static D3DXHANDLE m_SourceDimsEffectHandle;
static D3DXHANDLE m_TexelSizeEffectHandle;
static D3DXHANDLE m_SourceTextureEffectHandle;
static D3DXHANDLE m_WorkingTexture1EffectHandle;
static D3DXHANDLE m_WorkingTexture2EffectHandle;
static D3DXHANDLE m_Hq2xLookupTextureHandle;
// Technique stuff
static D3DXHANDLE m_PreprocessTechnique1EffectHandle;
static D3DXHANDLE m_PreprocessTechnique2EffectHandle;
static D3DXHANDLE m_CombineTechniqueEffectHandle;
enum psEffect_Pass { psEffect_None, psEffect_PreProcess1, psEffect_PreProcess2, psEffect_Combine };

static int postEffect_ParseParameters (LPD3DXEFFECTCOMPILER EffectCompiler, LPD3DXEFFECT effect)
{
	postSourceTextureHandle = effect->GetParameterByName (NULL, "SourceTexture");
	postMaskTextureHandle = effect->GetParameterByName (NULL, "OverlayTexture");
	postTechnique = effect->GetTechniqueByName ("PostTechnique");
	postTechniquePlain = effect->GetTechniqueByName ("PostTechniquePlain");
	postTechniqueAlpha = effect->GetTechniqueByName ("PostTechniqueAlpha");
	postMatrixSource = effect->GetParameterByName (NULL, "mtx");
	postMaskMult = effect->GetParameterByName (NULL, "maskmult");
	postMaskShift = effect->GetParameterByName (NULL, "maskshift");
	postFilterMode = effect->GetParameterByName (NULL, "filtermode");
	postTexelSize = effect->GetParameterByName (NULL, "texelsize");
	if (!postMaskShift || !postMaskMult || !postFilterMode || !postMatrixSource || !postTexelSize) {
		gui_message (_T("Mismatched _winuae.fx! Exiting.."));
		abort ();
	}
	return true;
}

static int psEffect_ParseParameters (LPD3DXEFFECTCOMPILER EffectCompiler, LPD3DXEFFECT effect)
{
	HRESULT hr = S_OK;
	// Look at parameters for semantics and annotations that we know how to interpret
	D3DXPARAMETER_DESC ParamDesc;
	D3DXPARAMETER_DESC AnnotDesc;
	D3DXHANDLE hParam;
	D3DXHANDLE hAnnot;
	LPDIRECT3DBASETEXTURE9 pTex = NULL;
	UINT iParam, iAnnot;

	if(effect == NULL)
		return 0;

	for(iParam = 0; iParam < EffectDesc.Parameters; iParam++) {
		LPCSTR pstrName = NULL;
		LPCSTR pstrFunction = NULL;
		D3DXHANDLE pstrFunctionHandle = NULL;
		LPCSTR pstrTarget = NULL;
		LPCSTR pstrTextureType = NULL;
		INT Width = D3DX_DEFAULT;
		INT Height= D3DX_DEFAULT;
		INT Depth = D3DX_DEFAULT;

		hParam = effect->GetParameter (NULL, iParam);
		hr = effect->GetParameterDesc (hParam, &ParamDesc);
		if (FAILED (hr)) {
			write_log (_T("GetParameterDescParm(%d) failed: %s\n"), D3DHEAD, iParam, D3DX_ErrorString (hr, NULL));
			return 0;
		}
		hr = S_OK;
		if(ParamDesc.Semantic != NULL) {
			if(ParamDesc.Class == D3DXPC_MATRIX_ROWS || ParamDesc.Class == D3DXPC_MATRIX_COLUMNS) {
				if(strcmpi(ParamDesc.Semantic, "world") == 0)
					m_MatWorldEffectHandle = hParam;
				else if(strcmpi(ParamDesc.Semantic, "view") == 0)
					m_MatViewEffectHandle = hParam;
				else if(strcmpi(ParamDesc.Semantic, "projection") == 0)
					m_MatProjEffectHandle = hParam;
				else if(strcmpi(ParamDesc.Semantic, "worldview") == 0)
					m_MatWorldViewEffectHandle = hParam;
				else if(strcmpi(ParamDesc.Semantic, "viewprojection") == 0)
					m_MatViewProjEffectHandle = hParam;
				else if(strcmpi(ParamDesc.Semantic, "worldviewprojection") == 0)
					m_MatWorldViewProjEffectHandle = hParam;
			} else if(ParamDesc.Class == D3DXPC_VECTOR && ParamDesc.Type == D3DXPT_FLOAT) {
				if(strcmpi(ParamDesc.Semantic, "sourcedims") == 0)
					m_SourceDimsEffectHandle = hParam;
				else if(strcmpi(ParamDesc.Semantic, "texelsize") == 0)
					m_TexelSizeEffectHandle = hParam;
			} else if(ParamDesc.Class == D3DXPC_SCALAR && ParamDesc.Type == D3DXPT_FLOAT) {
				if(strcmpi(ParamDesc.Semantic, "SCALING") == 0)
					hr = effect->GetFloat(hParam, &m_scale);
			} else if(ParamDesc.Class == D3DXPC_OBJECT && ParamDesc.Type == D3DXPT_TEXTURE) {
				if(strcmpi(ParamDesc.Semantic, "SOURCETEXTURE") == 0)
					m_SourceTextureEffectHandle = hParam;
				if(strcmpi(ParamDesc.Semantic, "WORKINGTEXTURE") == 0)
					m_WorkingTexture1EffectHandle = hParam;
				if(strcmpi(ParamDesc.Semantic, "WORKINGTEXTURE1") == 0)
					m_WorkingTexture2EffectHandle = hParam;
				if(strcmpi(ParamDesc.Semantic, "HQ2XLOOKUPTEXTURE") == 0)
					m_Hq2xLookupTextureHandle = hParam;
			} else if(ParamDesc.Class == D3DXPC_OBJECT && ParamDesc.Type == D3DXPT_STRING) {
				LPCSTR pstrTechnique = NULL;
				if(strcmpi(ParamDesc.Semantic, "COMBINETECHNIQUE") == 0) {
					hr = effect->GetString(hParam, &pstrTechnique);
					m_CombineTechniqueEffectHandle = effect->GetTechniqueByName(pstrTechnique);
				} else if(strcmpi(ParamDesc.Semantic, "PREPROCESSTECHNIQUE") == 0) {
					hr = effect->GetString(hParam, &pstrTechnique);
					m_PreprocessTechnique1EffectHandle = effect->GetTechniqueByName(pstrTechnique);
				} else if(strcmpi(ParamDesc.Semantic, "PREPROCESSTECHNIQUE1") == 0) {
					hr = effect->GetString(hParam, &pstrTechnique);
					m_PreprocessTechnique2EffectHandle = effect->GetTechniqueByName(pstrTechnique);
				} else if(strcmpi(ParamDesc.Semantic, "NAME") == 0) {
					hr = effect->GetString(hParam, &m_strName);
				}
			}
			if (FAILED (hr)) {
				write_log (_T("ParamDesc.Semantic failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, NULL));
				return 0;
			}
		}

		for(iAnnot = 0; iAnnot < ParamDesc.Annotations; iAnnot++) {
			hAnnot = effect->GetAnnotation (hParam, iAnnot);
			hr = effect->GetParameterDesc(hAnnot, &AnnotDesc);
			if (FAILED (hr)) {
				write_log (_T("GetParameterDescAnnot(%d) failed: %s\n"), D3DHEAD, iAnnot, D3DX_ErrorString (hr, NULL));
				return 0;
			}
			hr = S_OK;
			if(strcmpi(AnnotDesc.Name, "name") == 0) {
				hr = effect->GetString(hAnnot, &pstrName);
			} else if(strcmpi(AnnotDesc.Name, "function") == 0) {
				hr = effect->GetString(hAnnot, &pstrFunction);
				pstrFunctionHandle = effect->GetFunctionByName(pstrFunction);
			} else if(strcmpi(AnnotDesc.Name, "target") == 0) {
				hr = effect->GetString(hAnnot, &pstrTarget);
			} else if(strcmpi(AnnotDesc.Name, "width") == 0) {
				hr = effect->GetInt(hAnnot, &Width);
			} else if(strcmpi(AnnotDesc.Name, "height") == 0) {
				hr = effect->GetInt(hAnnot, &Height);
			} else if(strcmpi(AnnotDesc.Name, "depth") == 0) {
				hr = effect->GetInt(hAnnot, &Depth);
			} else if(strcmpi(AnnotDesc.Name, "type") == 0) {
				hr = effect->GetString(hAnnot, &pstrTextureType);
			}
			if (FAILED (hr)) {
				write_log (_T("GetString/GetInt(%d) failed: %s\n"), D3DHEAD, iAnnot, D3DX_ErrorString (hr, NULL));
				return 0;
			}
		}

		if(pstrFunctionHandle != NULL) {
			LPD3DXBUFFER pTextureShader = NULL;
			LPD3DXBUFFER lpErrors = 0;

			if(pstrTarget == NULL || strcmp(pstrTarget,"tx_1_1"))
				pstrTarget = "tx_1_0";

			if(SUCCEEDED(hr = EffectCompiler->CompileShader(
				pstrFunctionHandle, pstrTarget, D3DXSHADER_SKIPVALIDATION|D3DXSHADER_DEBUG, &pTextureShader, &lpErrors, NULL))) {
					LPD3DXTEXTURESHADER ppTextureShader;
					if (lpErrors)
						lpErrors->Release ();

					if(Width == D3DX_DEFAULT)
						Width = 64;
					if(Height == D3DX_DEFAULT)
						Height = 64;
					if(Depth == D3DX_DEFAULT)
						Depth = 64;

					D3DXCreateTextureShader((DWORD *)pTextureShader->GetBufferPointer(), &ppTextureShader);

					if(pstrTextureType != NULL) {
						if(strcmpi(pstrTextureType, "volume") == 0) {
							LPDIRECT3DVOLUMETEXTURE9 pVolumeTex = NULL;
							if(SUCCEEDED(hr = D3DXCreateVolumeTexture(d3ddev,
								Width, Height, Depth, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pVolumeTex))) {
									if(SUCCEEDED(hr = D3DXFillVolumeTextureTX(pVolumeTex, ppTextureShader))) {
										pTex = pVolumeTex;
									}
							}
						} else if(strcmpi(pstrTextureType, "cube") == 0) {
							LPDIRECT3DCUBETEXTURE9 pCubeTex = NULL;
							if(SUCCEEDED(hr = D3DXCreateCubeTexture(d3ddev,
								Width, D3DX_DEFAULT, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pCubeTex))) {
									if(SUCCEEDED(hr = D3DXFillCubeTextureTX(pCubeTex, ppTextureShader))) {
										pTex = pCubeTex;
									}
							}
						}
					} else {
						LPDIRECT3DTEXTURE9 p2DTex = NULL;
						if(SUCCEEDED(hr = D3DXCreateTexture(d3ddev, Width, Height,
							D3DX_DEFAULT, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &p2DTex))) {
								if(SUCCEEDED(hr = D3DXFillTextureTX(p2DTex, ppTextureShader))) {
									pTex = p2DTex;
								}
						}
					}
					effect->SetTexture(effect->GetParameter(NULL, iParam), pTex);
					if (pTex)
						pTex->Release ();
					if (pTextureShader)
						pTextureShader->Release ();
					if (ppTextureShader)
						ppTextureShader->Release ();
			} else {
				write_log (_T("%s: Could not compile texture shader: %s\n"), D3DHEAD, D3DX_ErrorString (hr, lpErrors));
				if (lpErrors)
					lpErrors->Release ();
				return 0;
			}
		}
	}
	return 1;
}

static int psEffect_hasPreProcess (void) { return m_PreprocessTechnique1EffectHandle != 0; }
static int psEffect_hasPreProcess2 (void) { return m_PreprocessTechnique2EffectHandle != 0; }

int D3D_goodenough (void)
{
	static int d3d_good;
	LPDIRECT3D9 d3dx;
	D3DCAPS9 d3dCaps;

	if (d3d_good > 0)
		return d3d_good;
	if (d3d_good < 0)
		return 0;
	d3d_good = -1;
	d3dx = Direct3DCreate9 (D3D_SDK_VERSION);
	if (d3dx != NULL) {
		if (SUCCEEDED (d3dx->GetDeviceCaps (D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &d3dCaps))) {
			if (((d3dCaps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) == D3DPTEXTURECAPS_NONPOW2CONDITIONAL) || !(d3dCaps.TextureCaps & D3DPTEXTURECAPS_POW2)) {
				if (!(d3dCaps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) && (d3dCaps.Caps2 & D3DCAPS2_DYNAMICTEXTURES)) {
					d3d_good = 1;
					if (d3dCaps.PixelShaderVersion >= D3DPS_VERSION(1, 0)) {
						d3d_good = 2;
						shaderon = 1;
					}
				}
			}
		}
		d3dx->Release ();
	}
#if SHADER == 0
	shaderon = 0;
#endif
	return d3d_good > 0 ? d3d_good : 0;
}

int D3D_canshaders (void)
{
	static int d3d_yesno = 0;
	HMODULE h;
	LPDIRECT3D9 d3dx;
	D3DCAPS9 d3dCaps;

	if (d3d_yesno < 0)
		return 0;
	if (d3d_yesno > 0)
		return 1;
	d3d_yesno = -1;
	h = LoadLibrary (D3DX9DLL);
	if (h != NULL) {
		FreeLibrary (h);
		d3dx = Direct3DCreate9 (D3D_SDK_VERSION);
		if (d3dx != NULL) {
			if (SUCCEEDED (d3dx->GetDeviceCaps (D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &d3dCaps))) {
				if (d3dCaps.PixelShaderVersion >= D3DPS_VERSION(2,0)) {
					write_log (_T("Direct3D: Pixel shader 2.0+ support detected, shader filters enabled.\n"));
					d3d_yesno = 1;
				}
			}
			d3dx->Release ();
		}
	}
	return d3d_yesno > 0 ? 1 : 0;
}

static const char *fx10 = {

"// 3 (version)\n"
"//\n"
"// WinUAE Direct3D post processing shader\n"
"//\n"
"// by Toni Wilen 2012\n"
"\n"
"uniform extern float4x4 mtx;\n"
"uniform extern float2 maskmult;\n"
"uniform extern float2 maskshift;\n"
"uniform extern int filtermode;\n"
"uniform extern float2 texelsize;\n"
"\n"
"// final possibly filtered Amiga output\n"
"texture SourceTexture : SOURCETEXTURE;\n"
"\n"
"sampler	SourceSampler = sampler_state {\n"
"	Texture	  = (SourceTexture);\n"
"	MinFilter = POINT;\n"
"	MagFilter = POINT;\n"
"	MipFilter = NONE;\n"
"	AddressU  = Clamp;\n"
"	AddressV  = Clamp;\n"
"};\n"
"\n"
"\n"
"texture OverlayTexture : OVERLAYTEXTURE;\n"
"\n"
"sampler	OverlaySampler = sampler_state {\n"
"	Texture	  = (OverlayTexture);\n"
"	MinFilter = POINT;\n"
"	MagFilter = POINT;\n"
"	MipFilter = NONE;\n"
"	AddressU  = Wrap;\n"
"	AddressV  = Wrap;\n"
"};\n"
"\n"
"struct VS_OUTPUT_POST\n"
"{\n"
"	float4 Position		: POSITION;\n"
"	float2 CentreUV		: TEXCOORD0;\n"
"	float2 Selector		: TEXCOORD1;\n"
"};\n"
"\n"
"VS_OUTPUT_POST VS_Post(float3 pos : POSITION, float2 TexCoord : TEXCOORD0)\n"
"{\n"
"	VS_OUTPUT_POST Out = (VS_OUTPUT_POST)0;\n"
"\n"
"	Out.Position = mul(float4(pos, 1.0f), mtx);\n"
"	Out.CentreUV = TexCoord;\n"
"	Out.Selector = TexCoord * maskmult + maskshift;\n"
"	return Out;\n"
"}\n"
"\n"
"float4 PS_Post(in VS_OUTPUT_POST inp) : COLOR\n"
"{\n"
"	float4 s = tex2D(SourceSampler, inp.CentreUV);\n"
"	float4 o = tex2D(OverlaySampler, inp.Selector);\n"
"	return s * o;\n"
"}\n"
"\n"
"float4 PS_PostAlpha(in VS_OUTPUT_POST inp) : COLOR\n"
"{\n"
"	float4 s = tex2D(SourceSampler, inp.CentreUV);\n"
"	float4 o = tex2D(OverlaySampler, inp.Selector);\n"
"	return s * (1 - o.a) + (o * o.a);\n"
"}\n"
"\n"
"float4 PS_PostPlain(in VS_OUTPUT_POST inp) : COLOR\n"
"{\n"
"	float4 s = tex2D(SourceSampler, inp.CentreUV);\n"
"	return s;\n"
"}\n"
"\n"
"// source and overlay texture\n"
"technique PostTechnique\n"
"{\n"
"    pass P0\n"
"    {\n"
"		VertexShader = compile vs_1_0 VS_Post();\n"
"		PixelShader  = compile ps_1_0 PS_Post();\n"
"    }  \n"
"}\n"
"\n"
"// source and scanline texture with alpha\n"
"technique PostTechniqueAlpha\n"
"{\n"
"	pass P0\n"
"	{\n"
"		VertexShader = compile vs_1_0 VS_Post();\n"
"		PixelShader  = compile ps_1_0 PS_PostAlpha();\n"
"    } \n"
"}\n"
"\n"
"// only source texture\n"
"technique PostTechniquePlain\n"
"{\n"
"	pass P0\n"
"	{\n"
"		VertexShader = compile vs_1_0 VS_Post();\n"
"		PixelShader  = compile ps_1_0 PS_PostPlain();\n"
"    }\n"
"}\n"
};

static const char *fx20 = {

"// 3 (version)\n"
"//\n"
"// WinUAE Direct3D post processing shader\n"
"//\n"
"// by Toni Wilen 2012\n"
"\n"
"uniform extern float4x4 mtx;\n"
"uniform extern float2 maskmult;\n"
"uniform extern float2 maskshift;\n"
"uniform extern int filtermode;\n"
"uniform extern float2 texelsize;\n"
"\n"
"// final possibly filtered Amiga output\n"
"texture SourceTexture : SOURCETEXTURE;\n"
"\n"
"sampler	SourceSampler = sampler_state {\n"
"	Texture	  = (SourceTexture);\n"
"	MinFilter = filtermode;\n"
"	MagFilter = filtermode;\n"
"	MipFilter = NONE;\n"
"	AddressU  = Clamp;\n"
"	AddressV  = Clamp;\n"
"};\n"
"\n"
"\n"
"texture OverlayTexture : OVERLAYTEXTURE;\n"
"\n"
"sampler	OverlaySampler = sampler_state {\n"
"	Texture	  = (OverlayTexture);\n"
"	MinFilter = POINT;\n"
"	MagFilter = POINT;\n"
"	MipFilter = NONE;\n"
"	AddressU  = Wrap;\n"
"	AddressV  = Wrap;\n"
"};\n"
"\n"
"struct VS_OUTPUT_POST\n"
"{\n"
"	float4 Position		: POSITION;\n"
"	float2 CentreUV		: TEXCOORD0;\n"
"	float2 Selector		: TEXCOORD1;\n"
"};\n"
"\n"
"VS_OUTPUT_POST VS_Post(float3 pos : POSITION, float2 TexCoord : TEXCOORD0)\n"
"{\n"
"	VS_OUTPUT_POST Out = (VS_OUTPUT_POST)0;\n"
"\n"
"	Out.Position = mul(float4(pos, 1.0f), mtx);\n"
"	Out.CentreUV = TexCoord;\n"
"	Out.Selector = TexCoord * maskmult + maskshift;\n"
"	return Out;\n"
"}\n"
"\n"
"float4 PS_Post(in VS_OUTPUT_POST inp) : COLOR\n"
"{\n"
"	float4 s = tex2D(SourceSampler, inp.CentreUV);\n"
"	float4 o = tex2D(OverlaySampler, inp.Selector);\n"
"	return s * o;\n"
"}\n"
"\n"
"float4 PS_PostAlpha(in VS_OUTPUT_POST inp) : COLOR\n"
"{\n"
"	float4 s = tex2D(SourceSampler, inp.CentreUV);\n"
"	float4 o = tex2D(OverlaySampler, inp.Selector);\n"
"	return s * (1 - o.a) + (o * o.a);\n"
"}\n"
"\n"
"float4 PS_PostPlain(in VS_OUTPUT_POST inp) : COLOR\n"
"{\n"
"	float4 s = tex2D(SourceSampler, inp.CentreUV);\n"
"	return s;\n"
"}\n"
"\n"
"// source and overlay texture\n"
"technique PostTechnique\n"
"{\n"
"    pass P0\n"
"    {\n"
"		VertexShader = compile vs_1_0 VS_Post();\n"
"		PixelShader  = compile ps_2_0 PS_Post();\n"
"    }  \n"
"}\n"
"\n"
"// source and scanline texture with alpha\n"
"technique PostTechniqueAlpha\n"
"{\n"
"	pass P0\n"
"	{\n"
"		VertexShader = compile vs_1_0 VS_Post();\n"
"		PixelShader  = compile ps_2_0 PS_PostAlpha();\n"
"    } \n"
"}\n"
"\n"
"// only source texture\n"
"technique PostTechniquePlain\n"
"{\n"
"	pass P0\n"
"	{\n"
"		VertexShader = compile vs_1_0 VS_Post();\n"
"		PixelShader  = compile ps_2_0 PS_PostPlain();\n"
"    }\n"
"}\n"
};	

static LPD3DXEFFECT psEffect_LoadEffect (const TCHAR *shaderfile, int full)
{
	int ret = 0;
	LPD3DXEFFECTCOMPILER EffectCompiler = NULL;
	LPD3DXBUFFER Errors = NULL;
	LPD3DXBUFFER BufferEffect = NULL;
	HRESULT hr;
	TCHAR tmp[MAX_DPATH], tmp2[MAX_DPATH], tmp3[MAX_DPATH];
	LPD3DXEFFECT effect = NULL;
	static int first;
	DWORD compileflags = psEnabled ? 0 : D3DXSHADER_USE_LEGACY_D3DX9_31_DLL;
	int canusefile = 0, existsfile = 0;
	bool plugin_path;

	compileflags |= EFFECTCOMPILERFLAGS;
	plugin_path = get_plugin_path (tmp, sizeof tmp / sizeof (TCHAR), _T("filtershaders\\direct3d"));
	_tcscpy (tmp3, tmp);
	_tcscat (tmp, shaderfile);
	if (!full) {
		struct zfile *z = zfile_fopen (tmp, _T("r"));
		if (z) {
			existsfile = 1;
			zfile_fgets (tmp2, sizeof tmp2 / sizeof (TCHAR), z);
			zfile_fclose (z);
			int ver = _tstol (tmp2 + 2);
			if (ver == EFFECT_VERSION) {
				canusefile = 1;
			} else {
				write_log (_T("'%s' mismatched version (%d != %d)\n"), tmp, ver, EFFECT_VERSION);
			}
		}
		hr = E_FAIL;
		if (canusefile) {
			write_log (_T("%s: Attempting to load '%s'\n"), D3DHEAD, tmp);
			hr = D3DXCreateEffectCompilerFromFile (tmp, NULL, NULL, compileflags, &EffectCompiler, &Errors);
			if (FAILED (hr))
				write_log (_T("%s: D3DXCreateEffectCompilerFromFile failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, Errors));
		}
		if (FAILED (hr)) {
			const char *str = psEnabled ? fx20 : fx10;
			int len = strlen (str);
			if (!existsfile && plugin_path) {
				struct zfile *z = zfile_fopen (tmp, _T("w"));
				if (z) {
					zfile_fwrite ((void*)str, len, 1, z);
					zfile_fclose (z);
				}
			}
			hr = D3DXCreateEffectCompiler (str, len, NULL, NULL, compileflags, &EffectCompiler, &Errors);
			if (FAILED (hr)) {
				write_log (_T("%s: D3DXCreateEffectCompilerFromResource failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, Errors));
				goto end;
			}
		}
	} else {
		write_log (_T("%s: Attempting to load '%s'\n"), D3DHEAD, tmp);
		hr = D3DXCreateEffectCompilerFromFile (tmp, NULL, NULL, compileflags, &EffectCompiler, &Errors);
		if (FAILED (hr)) {
			write_log (_T("%s: D3DXCreateEffectCompilerFromFile failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, Errors));
			goto end;
		}
	}

	if (Errors) {
		write_log (_T("%s: '%s' warning: %s\n"), D3DHEAD, shaderfile, D3DX_ErrorString (hr, Errors));
		Errors->Release();
		Errors = NULL;
	}

	hr = EffectCompiler->CompileEffect (0, &BufferEffect, &Errors);
	if (FAILED (hr)) {
		write_log (_T("%s: CompileEffect failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, Errors));
		goto end;
	}
	void *bp = BufferEffect->GetBufferPointer ();
	int bplen = BufferEffect->GetBufferSize ();
	hr = D3DXCreateEffect (d3ddev,
		bp, bplen,
		NULL, NULL,
		EFFECTCOMPILERFLAGS,
		NULL, &effect, &Errors);
	if (FAILED (hr)) {
		write_log (_T("%s: D3DXCreateEffect failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, Errors));
		goto end;
	}
	hr = effect->GetDesc (&EffectDesc);
	if (FAILED (hr)) {
		write_log (_T("%s: effect->GetDesc() failed: %s\n"), D3DHEAD, D3DX_ErrorString (hr, Errors));
		goto end;
	}
	if (full) {
		if (!psEffect_ParseParameters (EffectCompiler, effect))
			goto end;
	} else {
		if (!postEffect_ParseParameters (EffectCompiler, effect))
			goto end;
	}
	ret = 1;
	if (plugin_path && filenotificationhandle == NULL)
		filenotificationhandle  = FindFirstChangeNotification (tmp3, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
end:
	if (Errors)
		Errors->Release ();
	if (BufferEffect)
		BufferEffect->Release ();
	if (EffectCompiler)
		EffectCompiler->Release ();

	if (full) {
		psActive = FALSE;
		psPreProcess = FALSE;
		if (ret) {
			psActive = TRUE;
			if (psEffect_hasPreProcess ())
				psPreProcess = TRUE;
		}
	}

	if (ret)
		write_log (_T("%s: pixelshader filter '%s' enabled\n"), D3DHEAD, tmp);
	else
		write_log (_T("%s: pixelshader filter '%s' failed to initialize\n"), D3DHEAD, tmp);
	return effect;
}

static int psEffect_SetMatrices (D3DXMATRIXA16 *matProj, D3DXMATRIXA16 *matView, D3DXMATRIXA16 *matWorld)
{
	HRESULT hr;

	if (m_MatWorldEffectHandle) {
		hr = pEffect->SetMatrix (m_MatWorldEffectHandle, matWorld);
		if (FAILED (hr)) {
			write_log (_T("%s: Create:SetMatrix:matWorld %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_MatViewEffectHandle) {
		hr = pEffect->SetMatrix (m_MatViewEffectHandle, matView);
		if (FAILED (hr)) {
			write_log (_T("%s: Create:SetMatrix:matView %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_MatProjEffectHandle) {
		hr = pEffect->SetMatrix (m_MatProjEffectHandle, matProj);
		if (FAILED (hr)) {
			write_log (_T("%s: Create:SetMatrix:matProj %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_MatWorldViewEffectHandle) {
		D3DXMATRIXA16 matWorldView;
		D3DXMatrixMultiply (&matWorldView, matWorld, matView);
		hr = pEffect->SetMatrix (m_MatWorldViewEffectHandle, &matWorldView);
		if (FAILED (hr)) {
			write_log (_T("%s: Create:SetMatrix:matWorldView %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_MatViewProjEffectHandle) {
		D3DXMATRIXA16 matViewProj;
		D3DXMatrixMultiply (&matViewProj, matView, matProj);
		hr = pEffect->SetMatrix (m_MatViewProjEffectHandle, &matViewProj);
		if (FAILED (hr)) {
			write_log (_T("%s: Create:SetMatrix:matViewProj %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_MatWorldViewProjEffectHandle) {
		D3DXMATRIXA16 tmp, matWorldViewProj;
		D3DXMatrixMultiply (&tmp, matWorld, matView);
		D3DXMatrixMultiply (&matWorldViewProj, &tmp, matProj);
		hr = pEffect->SetMatrix (m_MatWorldViewProjEffectHandle, &matWorldViewProj);
		if (FAILED (hr)) {
			write_log (_T("%s: Create:SetMatrix:matWorldViewProj %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	return 1;
}

static int psEffect_SetTextures (LPDIRECT3DTEXTURE9 lpSource, LPDIRECT3DTEXTURE9 lpWorking1,
	LPDIRECT3DTEXTURE9 lpWorking2, LPDIRECT3DVOLUMETEXTURE9 lpHq2xLookupTexture)
{
	HRESULT hr;
	D3DXVECTOR4 fDims, fTexelSize;

	if (!m_SourceTextureEffectHandle) {
		write_log (_T("%s: Texture with SOURCETEXTURE semantic not found\n"), D3DHEAD);
		return 0;
	}
	hr = pEffect->SetTexture (m_SourceTextureEffectHandle, lpSource);
	if (FAILED (hr)) {
		write_log (_T("%s: SetTextures:lpSource %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	if (m_WorkingTexture1EffectHandle) {
		hr = pEffect->SetTexture (m_WorkingTexture1EffectHandle, lpWorking1);
		if (FAILED (hr)) {
			write_log (_T("%s: SetTextures:lpWorking1 %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_WorkingTexture2EffectHandle) {
		hr = pEffect->SetTexture (m_WorkingTexture2EffectHandle, lpWorking2);
		if (FAILED (hr)) {
			write_log (_T("%s: SetTextures:lpWorking2 %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_Hq2xLookupTextureHandle) {
		hr = pEffect->SetTexture (m_Hq2xLookupTextureHandle, lpHq2xLookupTexture);
		if (FAILED (hr)) {
			write_log (_T("%s: SetTextures:lpHq2xLookupTexture %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	fDims.x = 256; fDims.y = 256; fDims.z  = 1; fDims.w = 1;
	fTexelSize.x = 1; fTexelSize.y = 1; fTexelSize.z = 1; fTexelSize.w = 1; 
	if (lpSource) {
		D3DSURFACE_DESC Desc;
		lpSource->GetLevelDesc (0, &Desc);
		fDims.x = (FLOAT) Desc.Width;
		fDims.y = (FLOAT) Desc.Height;
	}

	fTexelSize.x = 1.0f / fDims.x;
	fTexelSize.y = 1.0f / fDims.y;

	if (m_SourceDimsEffectHandle) {
		hr = pEffect->SetVector (m_SourceDimsEffectHandle, &fDims);
		if (FAILED (hr)) {
			write_log (_T("%s: SetTextures:SetVector:Source %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}
	if (m_TexelSizeEffectHandle) {
		hr = pEffect->SetVector (m_TexelSizeEffectHandle, &fTexelSize);
		if (FAILED (hr)) {
			write_log (_T("%s: SetTextures:SetVector:Texel %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
	}

	return 1;
}

static int psEffect_Begin (LPD3DXEFFECT effect, enum psEffect_Pass pass, UINT *pPasses)
{
	HRESULT hr;
	switch (pass)
	{
	case psEffect_PreProcess1:
		hr = effect->SetTechnique (m_PreprocessTechnique1EffectHandle);
		break;
	case psEffect_PreProcess2:
		hr = effect->SetTechnique (m_PreprocessTechnique2EffectHandle);
		break;
	case psEffect_Combine:
		hr = effect->SetTechnique (m_CombineTechniqueEffectHandle);
		break;
	default:
		hr = S_OK;
		break;
	}
	if (FAILED (hr)) {
		write_log (_T("%s: SetTechnique: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	hr = effect->Begin (pPasses, 0);
	if (FAILED (hr)) {
		write_log (_T("%s: Begin: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	return 1;
}

static int psEffect_BeginPass (LPD3DXEFFECT effect, UINT Pass)
{
	HRESULT hr;

	hr = effect->BeginPass (Pass);
	if (FAILED (hr)) {
		write_log (_T("%s: BeginPass: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	return 1;
}
static int psEffect_EndPass (LPD3DXEFFECT effect)
{
	HRESULT hr;

	hr = effect->EndPass ();
	if (FAILED (hr)) {
		write_log (_T("%s: EndPass: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	return 1;
}
static int psEffect_End (LPD3DXEFFECT effect)
{
	HRESULT hr;

	hr = effect->End ();
	if (FAILED (hr)) {
		write_log (_T("%s: End: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	return 1;
}

static LPDIRECT3DTEXTURE9 createtext (int w, int h, D3DFORMAT format)
{
	LPDIRECT3DTEXTURE9 t;
	D3DLOCKED_RECT locked;
	HRESULT hr;

	hr = d3ddev->CreateTexture (w, h, 1, D3DUSAGE_DYNAMIC, format, D3DPOOL_DEFAULT, &t, NULL);
	if (FAILED (hr))
		write_log (_T("%s: CreateTexture() D3DUSAGE_DYNAMIC failed: %s (%d*%d %08x)\n"),
			D3DHEAD, D3D_ErrorString (hr), w, h, format);
	if (FAILED (hr)) {
		hr = d3ddev->CreateTexture (w, h, 1, 0, format, D3DPOOL_DEFAULT, &t, NULL);
	}
	if (FAILED (hr)) {
		write_log (_T("%s: CreateTexture() failed: %s (%d*%d %08x)\n"),
			D3DHEAD, D3D_ErrorString (hr), w, h, format);
		return 0;
	}
	hr = t->LockRect (0, &locked, NULL, 0);
	if (SUCCEEDED (hr)) {
		int y;
		int wb;
		wb = w * 4;
		if (wb > locked.Pitch)
			wb = w * 2;
		if (wb > locked.Pitch)
			wb = w * 1;
		for (y = 0; y < h; y++)
			memset ((uae_u8*)locked.pBits + y * locked.Pitch, 0, wb);
		t->UnlockRect (0);
	}
	return t;
}

static int worktex_width, worktex_height;

static int allocextratextures (int index, int w, int h)
{
	HRESULT hr;
	if (FAILED (hr = d3ddev->CreateTexture (w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &lpWorkTexture1[index], NULL))) {
		write_log (_T("%s: Failed to create temp texture: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	if (FAILED (hr = d3ddev->CreateTexture (w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &lpWorkTexture2[index], NULL))) {
		write_log (_T("%s: Failed to create working texture2: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	return 1;
}

static int createamigatexture (int w, int h)
{
	HRESULT hr;

	texture = createtext (w, h, tformat);
	if (!texture)
		return 0;
	write_log (_T("%s: %d*%d texture allocated, bits per pixel %d\n"), D3DHEAD, w, h, t_depth);
	if (psActive) {
		D3DLOCKED_BOX lockedBox;
		if (!allocextratextures (0, w, h))
			return 0;
		if (FAILED (hr = lpHq2xLookupTexture->LockBox (0, &lockedBox, NULL, 0))) {
			write_log (_T("%s: Failed to lock box of volume texture: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
		BuildHq2xLookupTexture (worktex_width, worktex_height, w, h,  (unsigned char*)lockedBox.pBits);
		lpHq2xLookupTexture->UnlockBox (0);
	}
	return 1;
}

static int createtexture (int ow, int oh, int win_w, int win_h)
{
	HRESULT hr;

	int w, h;
	if (ow > win_w * multx && oh > win_h * multx) {
		w = ow;
		h = oh;
	} else {
		w = win_w * multx;
		h = win_h * multx;
	}
	worktex_width = w;
	worktex_height = h;
	if (FAILED (hr = d3ddev->CreateTexture (w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &lpTempTexture, NULL))) {
		write_log (_T("%s: Failed to create working texture1: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return 0;
	}
	write_log (_T("%s: %d*%d working texture allocated, bits per pixel %d\n"), D3DHEAD, w, h, t_depth);
	texelsize.x = 1.0f / w; texelsize.y = 1.0f / h; texelsize.z = 1; texelsize.w = 1; 

	if (psActive) {
		if (extrapasses) {
			if (!allocextratextures (1, w, h))
				return 0;
		}
		if (FAILED (hr = d3ddev->CreateVolumeTexture (256, 16, 256, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &lpHq2xLookupTexture, NULL))) {
			write_log (_T("%s: Failed to create volume texture: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}

	}
	return 1;
}

static void updateleds (void)
{
	D3DLOCKED_RECT locked;
	HRESULT hr;
	static uae_u32 rc[256], gc[256], bc[256], a[256];
	static int done;
	int i, y;

	if (!done) {
		for (i = 0; i < 256; i++) {
			rc[i] = i << 16;
			gc[i] = i << 8;
			bc[i] = i << 0;
			a[i] = i << 24;
		}
		done = 1;
	}
	hr = ledtexture->LockRect (0, &locked, NULL, D3DLOCK_DISCARD);
	if (FAILED (hr)) {
		write_log (_T("%d: SL LockRect failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return;
	}
	for (y = 0; y < TD_TOTAL_HEIGHT; y++) {
		uae_u8 *buf = (uae_u8*)locked.pBits + y * locked.Pitch;
		memset (buf, 0, ledwidth * 4);
		draw_status_line_single (buf, 32 / 8, y, ledwidth, rc, gc, bc, a);
	}
	ledtexture->UnlockRect (0);
}

static int createledtexture (void)
{
	ledwidth = window_w;
	ledheight = TD_TOTAL_HEIGHT;
	ledtexture = createtext (ledwidth, ledheight, D3DFMT_A8R8G8B8);
	if (!ledtexture)
		return 0;
	return 1;
}

static int createsltexture (void)
{
	if (masktexture)
		return 0;
	sltexture = createtext (required_sl_texture_w, required_sl_texture_h, t_depth < 32 ? D3DFMT_A4R4G4B4 : D3DFMT_A8R8G8B8);
	if (!sltexture)
		return 0;
	write_log (_T("%s: SL %d*%d texture allocated\n"), D3DHEAD, required_sl_texture_w, required_sl_texture_h);
	maskmult_x = 1.0f;
	maskmult_y = 1.0f;
	return 1;
}

static void createscanlines (int force)
{
	HRESULT hr;
	D3DLOCKED_RECT locked;
	static int osl1, osl2, osl3;
	int sl4, sl42;
	int l1, l2;
	int x, y, yy;
	uae_u8 *sld, *p;
	int bpp;

	if (osl1 == currprefs.gfx_filter_scanlines && osl3 == currprefs.gfx_filter_scanlinelevel && osl2 == currprefs.gfx_filter_scanlineratio && !force)
		return;
	bpp = t_depth < 32 ? 2 : 4;
	osl1 = currprefs.gfx_filter_scanlines;
	osl3 = currprefs.gfx_filter_scanlinelevel;
	osl2 = currprefs.gfx_filter_scanlineratio;
	sl4 = currprefs.gfx_filter_scanlines * 16 / 100;
	sl42 = currprefs.gfx_filter_scanlinelevel * 16 / 100;
	if (sl4 > 15)
		sl4 = 15;
	if (sl42 > 15)
		sl42 = 15;
	l1 = (currprefs.gfx_filter_scanlineratio >> 0) & 15;
	l2 = (currprefs.gfx_filter_scanlineratio >> 4) & 15;

	if (l1 + l2 <= 0)
		return;

	if (!sltexture) {
		if (osl1 == 0 && osl3 == 0)
			return;
		if (!createsltexture ())
			return;
	}

	hr = sltexture->LockRect (0, &locked, NULL, 0);
	if (FAILED (hr)) {
		write_log (_T("%s: SL LockRect failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return;
	}
	sld = (uae_u8*)locked.pBits;
	for (y = 0; y < required_sl_texture_h; y++)
		memset (sld + y * locked.Pitch, 0, required_sl_texture_w * bpp);
	for (y = 1; y < required_sl_texture_h; y += l1 + l2) {
		for (yy = 0; yy < l2 && y + yy < required_sl_texture_h; yy++) {
			for (x = 0; x < required_sl_texture_w; x++) {
				uae_u8 sll = sl42;
				p = &sld[(y + yy) * locked.Pitch + (x * bpp)];
				if (bpp < 4) {
					/* 16-bit, A4R4G4B4 */
					p[1] = (sl4 << 4) | (sll << 0);
					p[0] = (sll << 4) | (sll << 0);
				} else {
					/* 32-bit, A8R8G8B8 */
					uae_u8 sll4 = sl4 | (sl4 << 4);
					uae_u8 sll2 = sll | (sll << 4);
					p[0] = sll2;
					p[1] = sll2;
					p[2] = sll2;
					p[3] = sll4;
				}
			}
		}
	}
	sltexture->UnlockRect (0);
}

static int findedge (D3DLOCKED_RECT *lock, int w, int h, int dx, int dy)
{
	int x = w / 2;
	int y = h / 2;
	
	if (dx != 0)
		x = dx < 0 ? 0 : w - 1;
	if (dy != 0)
		y = dy < 0 ? 0 : h - 1;
	
	for (;;) {
		uae_u32 *p = (uae_u32*)((uae_u8*)lock->pBits + y * lock->Pitch + x * 4);
		int alpha = (*p) >> 24;
		if (alpha != 255)
			break;
		x -= dx;
		y -= dy;
		if (x <= 0 || y <= 0)
			break;
		if (x >= w - 1 || y >= h - 1)
			break;
	}
	if (dx)
		return x;
	return y;
}

static int createmask2texture (const TCHAR *filename)
{
	struct zfile *zf;
	int size;
	uae_u8 *buf;
	LPDIRECT3DTEXTURE9 tx;
	HRESULT hr;
	D3DXIMAGE_INFO dinfo;
	TCHAR tmp[MAX_DPATH];

	if (mask2texture)
		mask2texture->Release();
	mask2texture = NULL;

	if (filename[0] == 0 || WIN32GFX_IsPicassoScreen ())
		return 0;

	zf = NULL;
	for (int i = 0; i < 2; i++) {
		if (i == 0) {
			get_plugin_path (tmp, sizeof tmp / sizeof (TCHAR), _T("overlays"));
			_tcscat (tmp, filename);
		} else {
			_tcscpy (tmp, filename);
		}
		TCHAR tmp2[MAX_DPATH], tmp3[MAX_DPATH];
		_tcscpy (tmp3, tmp);
		TCHAR *s = _tcsrchr (tmp3, '.');
		if (s) {
			TCHAR *s2 = s;
			while (s2 > tmp3) {
				TCHAR v = *s2;
				if (v == '_') {
					s = s2;
					break;
				}
				if (v == 'X' || v == 'x') {
					s2--;
					continue;
				}
				if (!_istdigit (v))
					break;
				s2--;
			}
			_tcscpy (tmp2, s);
			_stprintf (s, _T("_%dx%d%s"), window_w, window_h, tmp2);
			zf = zfile_fopen (tmp3, _T("rb"), ZFD_NORMAL);
			if (zf)
				break;
			float aspect = (float)window_w / window_h;
			int ax = -1, ay = -1;
			if (abs (aspect - 16.0 / 10.0) <= 0.1)
				ax = 16, ay = 10;
			if (abs (aspect - 16.0 / 9.0) <= 0.1)
				ax = 16, ay = 9;
			if (abs (aspect - 4.0 / 3.0) <= 0.1)
				ax = 4, ay = 3;
			if (ax > 0 && ay > 0) {
				_stprintf (s, _T("_%dx%d%s"), ax, ay, tmp2);
				zf = zfile_fopen (tmp3, _T("rb"), ZFD_NORMAL);
				if (zf)
					break;
			}
		}
		zf = zfile_fopen (tmp, _T("rb"), ZFD_NORMAL);
		if (zf)
			break;
	}
	if (!zf) {
		write_log (_T("%s: couldn't open overlay '%s'\n"), D3DHEAD, filename);
		return 0;
	}
	size = zfile_size (zf);
	buf = xmalloc (uae_u8, size);
	zfile_fread (buf, size, 1, zf);
	zfile_fclose (zf);
	hr = D3DXCreateTextureFromFileInMemoryEx (d3ddev, buf, size,
		 D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
		 D3DPOOL_DEFAULT, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, &dinfo, NULL, &tx);
	xfree (buf);
	if (FAILED (hr)) {
		write_log (_T("%s: overlay texture load failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		goto end;
	}
	mask2texture_w = dinfo.Width;
	mask2texture_h = dinfo.Height;
	mask2texture = tx;
	mask2rect.left = 0;
	mask2rect.top = 0;
	mask2rect.right = mask2texture_w;
	mask2rect.bottom = mask2texture_h;

	D3DLOCKED_RECT lock;
	if (SUCCEEDED (hr = mask2texture->LockRect (0, &lock, NULL, 0))) {
		mask2rect.left = findedge (&lock, mask2texture_w, mask2texture_h, -1, 0);
		mask2rect.right = findedge (&lock, mask2texture_w, mask2texture_h, 1, 0);
		mask2rect.top = findedge (&lock, mask2texture_w, mask2texture_h, 0, -1);
		mask2rect.bottom = findedge (&lock, mask2texture_w, mask2texture_h, 0, 1);
		mask2texture->UnlockRect (0);
	}
	if (mask2rect.left >= mask2texture_w / 2 || mask2rect.top >= mask2texture_h / 2 ||
		mask2rect.right <= mask2texture_w / 2 || mask2rect.bottom <= mask2texture_h / 2) {
		mask2rect.left = 0;
		mask2rect.top = 0;
		mask2rect.right = mask2texture_w;
		mask2rect.bottom = mask2texture_h;
	}
	mask2texture_multx = (float)window_w / mask2texture_w;
	mask2texture_multy = (float)window_h / mask2texture_h;
	mask2texture_offsetw = 0;

	if (isfullscreen () > 0) {
		struct MultiDisplay *md = getdisplay (&currprefs);
		float deskw = md->rect.right - md->rect.left;
		float deskh = md->rect.bottom - md->rect.top;
		//deskw = 800; deskh = 600;
		float dstratio = deskw / deskh;
		float srcratio = mask2texture_w / mask2texture_h;
		mask2texture_multx *= srcratio / dstratio;
	} else {
		mask2texture_multx = mask2texture_multy;
	}

	mask2texture_wh = window_h;
	mask2texture_ww = mask2texture_w * mask2texture_multx; 

	mask2texture_offsetw = (window_w - mask2texture_ww) / 2;

	if (mask2texture_offsetw > 0)
		blanktexture = createtext (mask2texture_offsetw + 1, window_h, D3DFMT_X8R8G8B8);

	float xmult = mask2texture_multx;
	float ymult = mask2texture_multy;

	mask2rect.left *= xmult;
	mask2rect.right *= xmult;
	mask2rect.top *= ymult;
	mask2rect.bottom *= ymult;
	mask2texture_wwx = mask2texture_w * xmult;
	if (mask2texture_wwx > window_w)
		mask2texture_wwx = window_w;
	if (mask2texture_wwx < mask2rect.right - mask2rect.left)
		mask2texture_wwx = mask2rect.right - mask2rect.left;
	if (mask2texture_wwx > mask2texture_ww)
		mask2texture_wwx = mask2texture_ww;

	mask2texture_minusx = - ((window_w - mask2rect.right) + mask2rect.left);
	if (mask2texture_offsetw > 0)
		mask2texture_minusx += mask2texture_offsetw * xmult;
	

	mask2texture_minusy = -(window_h - (mask2rect.bottom - mask2rect.top));

	mask2texture_hhx = mask2texture_h * ymult;

	write_log (_T("%s: overlay '%s' %.0f*%.0f (%d*%d - %d*%d) (%d*%d)\n"),
		D3DHEAD, tmp, mask2texture_w, mask2texture_h,
		mask2rect.left, mask2rect.top, mask2rect.right, mask2rect.bottom,
		mask2rect.right - mask2rect.left, mask2rect.bottom - mask2rect.top);

	return 1;
end:
	if (tx)
		tx->Release ();
	return 0;
}

static int createmasktexture (const TCHAR *filename)
{
	struct zfile *zf;
	int size;
	uae_u8 *buf;
	D3DSURFACE_DESC maskdesc, txdesc;
	LPDIRECT3DTEXTURE9 tx;
	HRESULT hr;
	D3DLOCKED_RECT lock, slock;
	D3DXIMAGE_INFO dinfo;
	TCHAR tmp[MAX_DPATH];
	int maskwidth, maskheight;

	if (filename[0] == 0)
		return 0;
	tx = NULL;
	get_plugin_path (tmp, sizeof tmp / sizeof (TCHAR), _T("masks"));
	_tcscat (tmp, filename);
	zf = zfile_fopen (tmp, _T("rb"), ZFD_NORMAL);
	if (!zf) {
		zf = zfile_fopen (filename, _T("rb"), ZFD_NORMAL);
		if (!zf) {
			write_log (_T("%s: couldn't open mask '%s'\n"), D3DHEAD, filename);
			return 0;
		}
	}
	size = zfile_size (zf);
	buf = xmalloc (uae_u8, size);
	zfile_fread (buf, size, 1, zf);
	zfile_fclose (zf);
	hr = D3DXCreateTextureFromFileInMemoryEx (d3ddev, buf, size,
		 D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8,
		 D3DPOOL_DEFAULT, D3DX_FILTER_NONE, D3DX_FILTER_NONE, 0, &dinfo, NULL, &tx);
	xfree (buf);
	if (FAILED (hr)) {
		write_log (_T("%s: temp mask texture load failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		goto end;
	}
	hr = tx->GetLevelDesc (0, &txdesc);
	if (FAILED (hr)) {
		write_log (_T("%s: mask image texture GetLevelDesc() failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		goto end;
	}
	masktexture_w = dinfo.Width;
	masktexture_h = dinfo.Height;
#if 0
	if (txdesc.Width == masktexture_w && txdesc.Height == masktexture_h && psEnabled) {
		// texture size == image size, no need to tile it (Wrap sampler does the rest)
		if (masktexture_w < window_w || masktexture_h < window_h) {
			maskwidth = window_w;
			maskheight = window_h;
		} else {
			masktexture = tx;
			tx = NULL;
		}
	} else {
#endif
		// both must be divisible by mask size
		maskwidth = ((window_w + masktexture_w - 1) / masktexture_w) * masktexture_w;
		maskheight = ((window_h + masktexture_h - 1) / masktexture_h) * masktexture_h;
#if 0
	}
#endif
	if (tx) {
		masktexture = createtext (maskwidth, maskheight, D3DFMT_X8R8G8B8);
		if (FAILED (hr)) {
			write_log (_T("%s: mask texture creation failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			goto end;
		}
		hr = masktexture->GetLevelDesc (0, &maskdesc);
		if (FAILED (hr)) {
			write_log (_T("%s: mask texture GetLevelDesc() failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			goto end;
		}
		if (SUCCEEDED (hr = masktexture->LockRect (0, &lock, NULL, 0))) {
			if (SUCCEEDED (hr = tx->LockRect (0, &slock, NULL, 0))) {
				int x, y, sx, sy;
				uae_u32 *sptr, *ptr;
				sy = 0;
				for (y = 0; y < maskdesc.Height; y++) {
					sx = 0;
					for (x = 0; x < maskdesc.Width; x++) {
						uae_u32 v;
						sptr = (uae_u32*)((uae_u8*)slock.pBits + sy * slock.Pitch + sx * 4);
						ptr = (uae_u32*)((uae_u8*)lock.pBits + y * lock.Pitch + x * 4);
						v = *sptr;
	//					v &= 0x00FFFFFF;
	//					v |= 0x80000000;
						*ptr = v;
						sx++;
						if (sx >= dinfo.Width)
							sx = 0;
					}
					sy++;
					if (sy >= dinfo.Height)
						sy = 0;
				}
				tx->UnlockRect (0);
			}
			masktexture->UnlockRect (0);
		}
		tx->Release ();
		masktexture_w = maskdesc.Width;
		masktexture_h = maskdesc.Height;
	}
	write_log (_T("%s: mask %d*%d (%d*%d) %d*%d ('%s') texture allocated\n"), D3DHEAD, masktexture_w, masktexture_h, txdesc.Width, txdesc.Height, maskdesc.Width, maskdesc.Height, filename);
	maskmult_x = (float)window_w / masktexture_w;
	maskmult_y = (float)window_h / masktexture_h;

	return 1;
end:
	if (masktexture)
		masktexture->Release ();
	masktexture = NULL;
	if (tx)
		tx->Release ();
	return 0;
}

bool getscalerect (float *mx, float *my, float *sx, float *sy)
{
	if (!mask2texture)
		return false;

	float mw = mask2rect.right - mask2rect.left;
	float mh = mask2rect.bottom - mask2rect.top;

	float mxt = (float)mw / gfxvidinfo.outbuffer->inwidth2;
	float myt = (float)mh / gfxvidinfo.outbuffer->inheight2;

	*mx = mask2texture_minusx / mxt;
	*my = mask2texture_minusy / myt;

	*sx = -((mask2texture_ww - mask2rect.right) - (mask2rect.left)) / 2;
	*sy = -((mask2texture_wh - mask2rect.bottom) - (mask2rect.top)) / 2;

	*sx /= mxt;
	*sy /= myt;

	return true;
}

static void setupscenecoords (void)
{
	RECT sr, dr, zr;
	float w, h;
	float dw, dh;
	static RECT sr2, dr2, zr2;

	//write_log (_T("%dx%d %dx%d %dx%d\n"), tin_w, tin_h, tin_w, tin_h, window_w, window_h);

	getfilterrect2 (&dr, &sr, &zr, window_w, window_h, tin_w / mult, tin_h / mult, mult, tin_w, tin_h);

	if (memcmp (&sr, &sr2, sizeof RECT) || memcmp (&dr, &dr2, sizeof RECT) || memcmp (&zr, &zr2, sizeof RECT)) {
		write_log (_T("POS (%d %d %d %d) - (%d %d %d %d)[%d,%d] (%d %d)\n"),
			dr.left, dr.top, dr.right, dr.bottom, sr.left, sr.top, sr.right, sr.bottom,
			sr.right - sr.left, sr.bottom - sr.top,
			zr.left, zr.top);
		sr2 = sr;
		dr2 = dr;
		zr2 = zr;
	}

	dw = dr.right - dr.left;
	dh = dr.bottom - dr.top;
	w = sr.right - sr.left;
	h = sr.bottom - sr.top;

	fakesize.x = w;
	fakesize.y = h;
	fakesize.w = 1;
	fakesize.z = 1;

	MatrixOrthoOffCenterLH (&m_matProj, 0, w + 0.05f, 0, h + 0.05f, 0.0f, 1.0f);

	float tx, ty;
	float sw, sh;

	if (0 && mask2texture) {

		float mw = mask2rect.right - mask2rect.left;
		float mh = mask2rect.bottom - mask2rect.top;

		tx = -0.5f + dw * tin_w / mw / 2;
		ty = +0.5f + dh * tin_h / mh / 2;

		float xshift = -zr.left;
		float yshift = -zr.top;

		sw = dw * tin_w / gfxvidinfo.outbuffer->inwidth2;
		sw *= mw / window_w;

		tx = -0.5f + window_w / 2;

		sh = dh * tin_h / gfxvidinfo.outbuffer->inheight2;
		sh *= mh / window_h;

		ty = +0.5f + window_h / 2;

		tx += xshift;
		ty += yshift;

	} else {

		tx = -0.5f + dw * tin_w / window_w / 2;
		ty = +0.5f + dh * tin_h / window_h / 2;

		float xshift = - zr.left - sr.left; // - (tin_w - 2 * zr.left - w),
		float yshift = + zr.top + sr.top - (tin_h - h);
	
		sw = dw * tin_w / window_w;
		sh = dh * tin_h / window_h;

		//sw -= 0.5f;
		//sh += 0.5f;

		tx += xshift;
		ty += yshift;

	}

	MatrixTranslation (&m_matView, tx, ty, 1.0f);

	MatrixScaling (&m_matWorld, sw + 0.5f / sw, sh + 0.5f / sh, 1.0f);

	cursor_offset_x = -zr.left;
	cursor_offset_y = -zr.top;

	//write_log (_T("%.1fx%.1f %.1fx%.1f %.1fx%.1f\n"), dw, dh, w, h, sw, sh);

	// ratio between Amiga texture and overlay mask texture
	float sw2 = dw * tin_w / window_w;
	float sh2 = dh * tin_h / window_h;

	//sw2 -= 0.5f;
	//sh2 += 0.5f;

	maskmult.x = sw2 * maskmult_x / w;
	maskmult.y = sh2 * maskmult_y / h;

	maskshift.x = 1.0f / maskmult_x;
	maskshift.y = 1.0f / maskmult_y;

	D3DXMATRIXA16 tmpmatrix;
	D3DXMatrixMultiply (&tmpmatrix, &m_matWorld, &m_matView);
	D3DXMatrixMultiply (&postproj, &tmpmatrix, &m_matProj);
}

uae_u8 *getfilterbuffer3d (struct vidbuffer *vb, int *widthp, int *heightp, int *pitch, int *depth)
{
	RECT dr, sr, zr;
	uae_u8 *p;
	int w, h;

	*depth = t_depth;
	getfilterrect2 (&dr, &sr, &zr, window_w, window_h, tin_w / mult, tin_h / mult, mult, tin_w, tin_h);
	w = sr.right - sr.left;
	h = sr.bottom - sr.top;
	p = vb->bufmem;
	if (pitch)
		*pitch = vb->rowbytes;
	p += (zr.top - h / 2) * vb->rowbytes + (zr.left - w / 2) * t_depth / 8;
	*widthp = w;
	*heightp = h;
	return p;
}

static void createvertex (void)
{
	HRESULT hr;
	struct TLVERTEX *vertices;
	float sizex, sizey;

	sizex = 1.0f;
	sizey = 1.0f;
	if (FAILED (hr = vertexBuffer->Lock (0, 0, (void**)&vertices, 0))) {
		write_log (_T("%s: Vertexbuffer lock failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return;
	}
	memset (vertices, 0, sizeof (struct TLVERTEX) * NUMVERTICES);
	//Setup vertices
	vertices[0].position.x = -0.5f; vertices[0].position.y = -0.5f;
	vertices[0].diffuse  = 0xFFFFFFFF;
	vertices[0].texcoord.x = 0.0f; vertices[0].texcoord.y = sizey;
	vertices[1].position.x = -0.5f; vertices[1].position.y = 0.5f;
	vertices[1].diffuse  = 0xFFFFFFFF;
	vertices[1].texcoord.x = 0.0f; vertices[1].texcoord.y = 0.0f;
	vertices[2].position.x = 0.5f; vertices[2].position.y = -0.5f;
	vertices[2].diffuse  = 0xFFFFFFFF;
	vertices[2].texcoord.x = sizex; vertices[2].texcoord.y = sizey;
	vertices[3].position.x = 0.5f; vertices[3].position.y = 0.5f;
	vertices[3].diffuse  = 0xFFFFFFFF;
	vertices[3].texcoord.x = sizex; vertices[3].texcoord.y = 0.0f;
	// Additional vertices required for some PS effects
	vertices[4].position.x = 0.0f; vertices[4].position.y = 0.0f;
	vertices[4].diffuse  = 0xFFFFFF00;
	vertices[4].texcoord.x = 0.0f; vertices[4].texcoord.y = 1.0f;
	vertices[5].position.x = 0.0f; vertices[5].position.y = 1.0f;
	vertices[5].diffuse  = 0xFFFFFF00;
	vertices[5].texcoord.x = 0.0f; vertices[5].texcoord.y = 0.0f;
	vertices[6].position.x = 1.0f; vertices[6].position.y = 0.0f;
	vertices[6].diffuse  = 0xFFFFFF00;
	vertices[6].texcoord.x = 1.0f; vertices[6].texcoord.y = 1.0f;
	vertices[7].position.x = 1.0f; vertices[7].position.y = 1.0f;
	vertices[7].diffuse  = 0xFFFFFF00;
	vertices[7].texcoord.x = 1.0f; vertices[7].texcoord.y = 0.0f;
	if (FAILED(hr = vertexBuffer->Unlock ()))
		write_log (_T("%s: Vertexbuffer unlock failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
}

static void settransform (void)
{
	// Projection is (0,0,0) -> (1,1,1)
	MatrixOrthoOffCenterLH (&m_matPreProj, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);
	// Align texels with pixels
	MatrixTranslation (&m_matPreView, -0.5f / tout_w, 0.5f / tout_h, 0.0f);
	// Identity for world
	D3DXMatrixIdentity (&m_matPreWorld);
	psEffect_SetMatrices (&m_matProj, &m_matView, &m_matWorld);

	MatrixOrthoOffCenterLH (&m_matProj2, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f);

	MatrixTranslation (&m_matView2, 0.5f - 0.5f / tout_w, 0.5f + 0.5f / tout_h, 0.0f);
	D3DXMatrixIdentity (&m_matWorld2);
}

static void freetextures (void)
{
	if (texture) {
		texture->Release ();
		texture = NULL;
	}
	if (lpTempTexture) {
		lpTempTexture->Release ();
		lpTempTexture = NULL;
	}
	for (int i = 0; i < MAX_PASSES; i++) {
		if (lpWorkTexture1[i]) {
			lpWorkTexture1[i]->Release ();
			lpWorkTexture1[i] = NULL;
		}
		if (lpWorkTexture2[i]) {
			lpWorkTexture2[i]->Release ();
			lpWorkTexture2[i] = NULL;
		}
	}
	if (lpHq2xLookupTexture) {
		lpHq2xLookupTexture->Release ();
		lpHq2xLookupTexture = NULL;
	}
}

static void getswapchain (void)
{
	if (!d3dswapchain) {
		HRESULT hr = d3ddev->GetSwapChain (0, &d3dswapchain);
		if (FAILED (hr)) {
			write_log (_T("%s: GetSwapChain() failed, %s\n"), D3DHEAD, D3D_ErrorString (hr));
		}
	}
}

static void invalidatedeviceobjects (void)
{
	if (filenotificationhandle  != NULL)
		FindCloseChangeNotification  (filenotificationhandle);
	filenotificationhandle = NULL;
	freetextures ();
	if (query) {
		query->Release();
		query = NULL;
	}
	if (sprite) {
		sprite->Release ();
		sprite = NULL;
	}
	if (ledtexture) {
		ledtexture->Release ();
		ledtexture = NULL;
	}
	if (sltexture) {
		sltexture->Release ();
		sltexture = NULL;
	}
	if (masktexture) {
		masktexture->Release ();
		masktexture = NULL;
	}
	if (mask2texture) {
		mask2texture->Release ();
		mask2texture = NULL;
	}
	if (blanktexture) {
		blanktexture->Release ();
		blanktexture = NULL;
	}
	if (cursorsurfaced3d) {
		cursorsurfaced3d->Release ();
		cursorsurfaced3d = NULL;
	}
	if (pEffect) {
		pEffect->Release ();
		pEffect = NULL;
	}
	if (postEffect) {
		postEffect->Release ();
		postEffect = NULL;
	}
	if (d3ddev)
		d3ddev->SetStreamSource (0, NULL, 0, 0);
	if (vertexBuffer) {
		vertexBuffer->Release ();
		vertexBuffer = NULL;
	}
	if (d3dswapchain)  {
		d3dswapchain->Release ();
		d3dswapchain = NULL;
	}
	m_MatWorldEffectHandle = NULL;
	m_MatViewEffectHandle = NULL;
	m_MatProjEffectHandle = NULL;
	m_MatWorldViewEffectHandle = NULL;
	m_MatViewProjEffectHandle = NULL;
	m_MatWorldViewProjEffectHandle = NULL;
	m_SourceDimsEffectHandle = NULL;
	m_TexelSizeEffectHandle = NULL;
	m_SourceTextureEffectHandle = NULL;
	m_WorkingTexture1EffectHandle = NULL;
	m_WorkingTexture2EffectHandle = NULL;
	m_Hq2xLookupTextureHandle = NULL;
	m_PreprocessTechnique1EffectHandle = NULL;
	m_PreprocessTechnique2EffectHandle = NULL;
	m_CombineTechniqueEffectHandle = NULL;
	locked = 0;
	maskshift.x = maskshift.y = maskshift.z = maskshift.w = 0;
	maskmult.x = maskmult.y = maskmult.z = maskmult.w = 0;
}

static int restoredeviceobjects (void)
{
	int vbsize;
	int wasshader = shaderon;
	HRESULT hr;

	invalidatedeviceobjects ();
	getswapchain ();

	while (shaderon > 0) {
		postEffect = psEffect_LoadEffect (psEnabled ? _T("_winuae.fx") : _T("_winuae_old.fx"), false);
		if (!postEffect) {
			shaderon = 0;
			break;
		}
		if (currprefs.gfx_filtershader[0]) {
			if (!(pEffect = psEffect_LoadEffect (currprefs.gfx_filtershader, true))) {
				currprefs.gfx_filtershader[0] = changed_prefs.gfx_filtershader[0] = 0;
				break;
			}
		}
		if (currprefs.gfx_filter_scanlines > 0) {
			createsltexture ();
			createscanlines (1);
		}
		createmasktexture (currprefs.gfx_filtermask);
		break;
	}
	if (wasshader && !shaderon)
		write_log (_T("Falling back to non-shader mode\n"));

	createmask2texture (currprefs.gfx_filteroverlay);

	createledtexture ();

	hr = D3DXCreateSprite (d3ddev, &sprite);
	if (FAILED (hr)) {
		write_log (_T("%s: D3DXSprite failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
	}

	int curw = CURSORMAXWIDTH, curh = CURSORMAXHEIGHT;
	cursorsurfaced3d = createtext (curw, curh, D3DFMT_A8R8G8B8);
	cursor_v = false;

	vbsize = sizeof (struct TLVERTEX) * NUMVERTICES;
	if (FAILED (hr = d3ddev->CreateVertexBuffer (vbsize, D3DUSAGE_WRITEONLY,
		D3DFVF_TLVERTEX, D3DPOOL_DEFAULT, &vertexBuffer, NULL))) {
			write_log (_T("%s: failed to create vertex buffer: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
	}
	createvertex ();
	if (FAILED (hr = d3ddev->SetFVF (D3DFVF_TLVERTEX)))
		write_log (_T("%s: SetFVF failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
	if (FAILED (hr = d3ddev->SetStreamSource (0, vertexBuffer, 0, sizeof (struct TLVERTEX))))
		write_log (_T("%s: SetStreamSource failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));

	hr = d3ddev->SetRenderState (D3DRS_CULLMODE, D3DCULL_NONE);
	hr = d3ddev->SetRenderState (D3DRS_LIGHTING, FALSE);

	settransform ();

	return 1;
}

static void D3D_free2 (void)
{
	invalidatedeviceobjects ();
	if (d3ddev) {
		d3ddev->Release ();
		d3ddev = NULL;
	}
	if (d3d) {
		d3d->Release ();
		d3d = NULL;
	}
	d3d_enabled = 0;
	psPreProcess = 0;
	psActive = 0;
	resetcount = 0;
	devicelost = 0;
	renderdisabled = false;
	changed_prefs.leds_on_screen = currprefs.leds_on_screen = currprefs.leds_on_screen & ~STATUSLINE_TARGET;
}

void D3D_free (void)
{
	D3D_free2 ();
	ddraw_fs_hack_free ();
}

#define VBLANKDEBUG 0

bool D3D_getvblankpos (int *vpos)
{
	HRESULT hr;
	D3DRASTER_STATUS rt;
#if VBLANKDEBUG
	static UINT lastline;
	static BOOL lastinvblank;
#endif
	*vpos = -2;
	if (!isd3d ())
		return false;
	if (d3dswapchain)
		hr = d3dswapchain->GetRasterStatus (&rt);
	else
		hr = d3ddev->GetRasterStatus (0, &rt);
	if (FAILED (hr)) {
		write_log (_T("%s: GetRasterStatus %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return false;
	}
	if (rt.ScanLine > maxscanline)
		maxscanline = rt.ScanLine;
	*vpos = rt.ScanLine;
#if VBLANKDEBUG
	if (lastline != rt.ScanLine || lastinvblank != rt.InVBlank) {
		write_log(_T("%d:%d "), rt.InVBlank ? 1 : 0, rt.ScanLine);
		lastline = rt.ScanLine;
		lastinvblank = rt.InVBlank;
	}
#endif
	if (rt.InVBlank != 0)
		*vpos = -1;
	return true;
}

void D3D_vblank_reset (double freq)
{
	if (!isd3d ())
		return;
}

static int getd3dadapter (IDirect3D9 *d3d)
{
	struct MultiDisplay *md = getdisplay (&currprefs);
	int num = d3d->GetAdapterCount ();
	HMONITOR winmon;
	POINT pt;

	pt.x = (md->rect.right - md->rect.left) / 2 + md->rect.left;
	pt.y = (md->rect.bottom - md->rect.top) / 2 + md->rect.top;
	winmon = MonitorFromPoint (pt, MONITOR_DEFAULTTONEAREST);
	for (int i = 0; i < num; i++) {
		HMONITOR d3dmon = d3d->GetAdapterMonitor (i);
		if (d3dmon == winmon)
			return i;
	}
	return D3DADAPTER_DEFAULT;
}

const TCHAR *D3D_init (HWND ahwnd, int w_w, int w_h, int depth, int mmult)
{
	HRESULT ret, hr;
	static TCHAR errmsg[100] = { 0 };
	D3DDISPLAYMODE mode;
	D3DCAPS9 d3dCaps;
	int adapter;
	DWORD flags;
	HINSTANCE d3dDLL, d3dx;
	typedef HRESULT (WINAPI *LPDIRECT3DCREATE9EX)(UINT, IDirect3D9Ex**);
	LPDIRECT3DCREATE9EX d3dexp = NULL;
	int vsync = isvsync ();
	struct apmode *ap = picasso_on ? &currprefs.gfx_apmode[APMODE_RTG] : &currprefs.gfx_apmode[APMODE_NATIVE];
	D3DADAPTER_IDENTIFIER9 did;

	D3D_free2 ();
	if (!currprefs.gfx_api) {
		_tcscpy (errmsg, _T("D3D: not enabled"));
		return errmsg;
	}

	xfree (fakebitmap);
	fakebitmap = xmalloc (uae_u8, w_w * depth);

	d3dx = LoadLibrary (D3DX9DLL);
	if (d3dx == NULL) {
		_tcscpy (errmsg, _T("Direct3D: Newer DirectX Runtime required.\n\nhttp://go.microsoft.com/fwlink/?linkid=56513"));
		if (isfullscreen () <= 0)
			ShellExecute(NULL, _T("open"), _T("http://go.microsoft.com/fwlink/?linkid=56513"), NULL, NULL, SW_SHOWNORMAL);
		return errmsg;
	}
	FreeLibrary (d3dx);

	D3D_goodenough ();
	D3D_canshaders ();

	d3d_ex = FALSE;
	d3dDLL = LoadLibrary (_T("D3D9.DLL"));
	if (d3dDLL == NULL) {
		_tcscpy (errmsg, _T("Direct3D: DirectX 9 or newer required"));
		return errmsg;
	} else {
		d3dexp  = (LPDIRECT3DCREATE9EX)GetProcAddress (d3dDLL, "Direct3DCreate9Ex");
		if (d3dexp)
			d3d_ex = TRUE;
	}
	FreeLibrary (d3dDLL);
	hr = -1;
	if (d3d_ex && D3DEX) {
		hr = d3dexp (D3D_SDK_VERSION, &d3dex);
		if (FAILED (hr))
			write_log (_T("Direct3D: failed to create D3DEx object: %s\n"), D3D_ErrorString (hr));
		d3d = (IDirect3D9*)d3dex;
	}
	if (FAILED (hr)) {
		d3d_ex = 0;
		d3dex = NULL;
		d3d = Direct3DCreate9 (D3D_SDK_VERSION);
		if (d3d == NULL) {
			D3D_free ();
			_tcscpy (errmsg, _T("Direct3D: failed to create D3D object"));
			return errmsg;
		}
	}
	if (d3d_ex)
		D3DHEAD = _T("D3D9Ex");
	else
		D3DHEAD = _T("D3D9");


	adapter = getd3dadapter (d3d);

	modeex.Size = sizeof modeex;
	if (d3dex && D3DEX) {
		LUID luid;
		hr = d3dex->GetAdapterLUID (adapter, &luid);
		hr = d3dex->GetAdapterDisplayModeEx (adapter, &modeex, NULL);
	}
	if (FAILED (hr = d3d->GetAdapterDisplayMode (adapter, &mode)))
		write_log (_T("%s: GetAdapterDisplayMode failed %s\n"), D3DHEAD, D3D_ErrorString (hr));
	if (FAILED (hr = d3d->GetDeviceCaps (adapter, D3DDEVTYPE_HAL, &d3dCaps)))
		write_log (_T("%s: GetDeviceCaps failed %s\n"), D3DHEAD, D3D_ErrorString (hr));
	if (SUCCEEDED (hr = d3d->GetAdapterIdentifier (adapter, 0, &did))) {
		TCHAR *s = au (did.Description);
		write_log (_T("Device name: '%s' %llx.%x\n"), s, did.DriverVersion, did.Revision);
		xfree (s);
	}

	memset (&dpp, 0, sizeof (dpp));
	dpp.Windowed = isfullscreen () <= 0;
	dpp.BackBufferFormat = mode.Format;
	dpp.BackBufferCount = ap->gfx_backbuffers;
	dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	dpp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
	dpp.BackBufferWidth = w_w;
	dpp.BackBufferHeight = w_h;
	dpp.PresentationInterval = !ap->gfx_vflip ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;

	modeex.Width = w_w;
	modeex.Height = w_h;
	modeex.RefreshRate = 0;
	modeex.ScanLineOrdering = ap->gfx_interlaced ? D3DSCANLINEORDERING_INTERLACED : D3DSCANLINEORDERING_PROGRESSIVE;
	modeex.Format = mode.Format;

	vsync2 = 0;
	int hzmult = 0;
	if (isfullscreen () > 0) {
		dpp.FullScreen_RefreshRateInHz = getrefreshrate (modeex.Width, modeex.Height);
		modeex.RefreshRate = dpp.FullScreen_RefreshRateInHz;
		if (vsync > 0) {
			dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
			getvsyncrate (dpp.FullScreen_RefreshRateInHz, &hzmult);
			if (hzmult < 0) {
				if (d3dCaps.PresentationIntervals & D3DPRESENT_INTERVAL_TWO)
					dpp.PresentationInterval = D3DPRESENT_INTERVAL_TWO;
				else
					vsync2 = -1;
			} else if (hzmult > 0) {
				vsync2 = 1;
			}
		}
	}
	if (vsync < 0) {
		vsync2 = 0;
		getvsyncrate (dpp.FullScreen_RefreshRateInHz, &hzmult);
		if (hzmult > 0) {
			vsync2 = 1;
		} else if (hzmult < 0 && ap->gfx_vflip) {
			if (d3dCaps.PresentationIntervals & D3DPRESENT_INTERVAL_TWO)
				dpp.PresentationInterval = D3DPRESENT_INTERVAL_TWO;
			else
				vsync2 = -1;
		}
	}

	d3dhwnd = ahwnd;
	t_depth = depth;

	flags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
	// Check if hardware vertex processing is available
	if(d3dCaps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) {
		flags |= D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE;
	} else {
		flags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
	}
	if (d3d_ex && D3DEX) {
		ret = d3dex->CreateDeviceEx (adapter, D3DDEVTYPE_HAL, d3dhwnd, flags, &dpp, dpp.Windowed ? NULL : &modeex, &d3ddevex);
		d3ddev = d3ddevex;
	} else {
		ret = d3d->CreateDevice (adapter, D3DDEVTYPE_HAL, d3dhwnd, flags, &dpp, &d3ddev);
	}
	if (FAILED (ret) && (flags & D3DCREATE_PUREDEVICE)) {
		flags &= ~D3DCREATE_PUREDEVICE;
		if (d3d_ex && D3DEX) {
			ret = d3dex->CreateDeviceEx (adapter, D3DDEVTYPE_HAL, d3dhwnd, flags, &dpp, dpp.Windowed ? NULL : &modeex, &d3ddevex);
			d3ddev = d3ddevex;
		} else {
			ret = d3d->CreateDevice (adapter, D3DDEVTYPE_HAL, d3dhwnd, flags, &dpp, &d3ddev);
		}
		if (FAILED (ret) && (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING)) {
			flags &= ~D3DCREATE_HARDWARE_VERTEXPROCESSING;
			flags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
			if (d3d_ex && D3DEX) {
				ret = d3dex->CreateDeviceEx (adapter, D3DDEVTYPE_HAL, d3dhwnd, flags, &dpp, dpp.Windowed ? NULL : &modeex, &d3ddevex);
				d3ddev = d3ddevex;
			} else {
				ret = d3d->CreateDevice (adapter, D3DDEVTYPE_HAL, d3dhwnd, flags, &dpp, &d3ddev);
			}
		}
	}

	if (FAILED (ret)) {
		_stprintf (errmsg, _T("%s failed, %s\n"), d3d_ex && D3DEX ? _T("CreateDeviceEx") : _T("CreateDevice"), D3D_ErrorString (ret));
		if (ret == D3DERR_INVALIDCALL && dpp.Windowed == 0 && dpp.FullScreen_RefreshRateInHz && !ddraw_fs) {
			write_log (_T("%s\n"), errmsg);
			write_log (_T("%s: Retrying fullscreen with DirectDraw\n"), D3DHEAD);
			if (ddraw_fs_hack_init ()) {
				const TCHAR *err2 = D3D_init (ahwnd, w_w, w_h, depth, mmult);
				if (err2)
					ddraw_fs_hack_free ();
				return err2;
			}
		}
		if (d3d_ex && D3DEX) {
			write_log (_T("%s\n"), errmsg);
			D3DEX = 0;
			return D3D_init (ahwnd, w_w, w_h, depth, mmult);
		}
		D3D_free ();
		return errmsg;
	}

	if (d3dCaps.PixelShaderVersion >= D3DPS_VERSION(2, 0))
		psEnabled = TRUE;
	else
		psEnabled = FALSE;

	max_texture_w = d3dCaps.MaxTextureWidth;
	max_texture_h = d3dCaps.MaxTextureHeight;

	write_log (_T("%s: %08X "), D3DHEAD, flags, d3dCaps.Caps, d3dCaps.Caps2, d3dCaps.Caps3);
	if (d3dCaps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY)
		write_log (_T("SQUAREONLY "));
	if (d3dCaps.TextureCaps & D3DPTEXTURECAPS_POW2)
		write_log (_T("POW2 "));
	if (d3dCaps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL)
		write_log (_T("NPOTCONDITIONAL "));
	if (d3dCaps.TextureCaps & D3DPTEXTURECAPS_ALPHA)
		write_log (_T("ALPHA "));
	if (d3dCaps.Caps2 & D3DCAPS2_DYNAMICTEXTURES)
		write_log (_T("DYNAMIC "));
	if (d3dCaps.Caps & D3DCAPS_READ_SCANLINE)
		write_log (_T("SCANLINE "));
	
	write_log (_T("\n"));

	write_log (_T("%s: PS=%d.%d VS=%d.%d %d*%d*%d%s%s VS=%d B=%d%s %d-bit %d\n"),
		D3DHEAD,
		(d3dCaps.PixelShaderVersion >> 8) & 0xff, d3dCaps.PixelShaderVersion & 0xff,
		(d3dCaps.VertexShaderVersion >> 8) & 0xff, d3dCaps.VertexShaderVersion & 0xff,
		modeex.Width, modeex.Height,
		dpp.FullScreen_RefreshRateInHz,
		ap->gfx_interlaced ? _T("i") : _T("p"),
		dpp.Windowed ? _T("") : _T(" FS"),
		vsync, ap->gfx_backbuffers,
		ap->gfx_vflip < 0 ? _T("WE") : (ap->gfx_vflip > 0 ? _T("WS") :  _T("I")), 
		t_depth, adapter
	);

	if ((d3dCaps.PixelShaderVersion < D3DPS_VERSION(2,0) || !psEnabled || max_texture_w < 2048 || max_texture_h < 2048 || (!shaderon && SHADER > 0)) && d3d_ex) {
		D3DEX = 0;
		write_log (_T("Disabling D3D9Ex\n"));
		if (d3ddev) {
			d3ddev->Release ();
			d3ddev = NULL;
		}
		if (d3d) {
			d3d->Release ();
			d3d = NULL;
		}
		d3ddevex = NULL;
		return D3D_init (ahwnd, w_w, w_h, depth, mmult);
	}
	if (!shaderon)
		write_log (_T("Using non-shader version\n"));

	multx = mmult;
	mult = S2X_getmult ();

	window_w = w_w;
	window_h = w_h;

	if (max_texture_w < w_w  || max_texture_h < w_h) {
		_stprintf (errmsg, _T("%s: %d * %d or bigger texture support required\nYour card's maximum texture size is only %d * %d"),
			D3DHEAD, w_w, w_h, max_texture_w, max_texture_h);
		return errmsg;
	}
	while (multx > 1 && (w_w * multx > max_texture_w || w_h * multx > max_texture_h))
		multx--;

	required_sl_texture_w = w_w;
	required_sl_texture_h = w_h;
	if (currprefs.gfx_filter_scanlines > 0 && (max_texture_w < w_w || max_texture_h < w_h)) {
		gui_message (_T("%s: %d * %d or bigger texture support required for scanlines (max is only %d * %d)\n"),
			D3DHEAD, _T("Scanlines disabled."),
			required_sl_texture_w, required_sl_texture_h, max_texture_w, max_texture_h);
		changed_prefs.gfx_filter_scanlines = currprefs.gfx_filter_scanlines = 0;
	}

	switch (depth)
	{
		case 32:
		default:
			tformat = D3DFMT_X8R8G8B8;
		break;
		case 15:
			tformat = D3DFMT_X1R5G5B5;
		break;
		case 16:
			tformat = D3DFMT_R5G6B5;
		break;
	}

	changed_prefs.leds_on_screen = currprefs.leds_on_screen = currprefs.leds_on_screen | STATUSLINE_TARGET;

	if (!restoredeviceobjects ()) {
		D3D_free ();
		_stprintf (errmsg, _T("%s: initialization failed."), D3DHEAD);
		return errmsg;
	}
	maxscanline = 0;
	d3d_enabled = 1;
	wasstilldrawing_broken = true;

	if (vsync < 0 && ap->gfx_vflip == 0) {
		hr = d3ddev->CreateQuery(D3DQUERYTYPE_EVENT, &query);
		if (FAILED (hr))
			write_log (_T("%s: CreateQuery(D3DQUERYTYPE_EVENT) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
	}
	if (d3ddevex) {
		UINT v = 12345;
		hr = d3ddevex->GetMaximumFrameLatency (&v);
		if (FAILED (hr)) {
			write_log (_T("%s: GetMaximumFrameLatency() failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			v = 1;
		}
		hr = S_OK;
		if (forcedframelatency >= 0)
			hr = d3ddevex->SetMaximumFrameLatency (forcedframelatency);
		else if (v > 1 || !vsync)
			hr = d3ddevex->SetMaximumFrameLatency (vsync ? 1 : 0);
		if (FAILED (hr))
			write_log (_T("%s: SetMaximumFrameLatency() failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
	}

	return 0;
}

static bool alloctextures (void)
{
	if (!createtexture (tout_w, tout_h, window_w, window_h))
		return false;
	if (!createamigatexture (tin_w, tin_h))
		return false;
	return true;
}

bool D3D_alloctexture (int w, int h)
{
	tin_w = w * mult;
	tin_h = h * mult;

	tout_w = tin_w * multx;
	tout_h = tin_h * multx;

	changed_prefs.leds_on_screen = currprefs.leds_on_screen = currprefs.leds_on_screen | STATUSLINE_TARGET;

	freetextures ();
	return alloctextures ();
}


static HRESULT reset (void)
{
	HRESULT hr;
	bool oldrender = renderdisabled;
	renderdisabled = true;
	if (d3dex)
		hr = d3ddevex->ResetEx (&dpp, dpp.Windowed ? NULL : &modeex);
	else
		hr = d3ddev->Reset (&dpp);
	renderdisabled = oldrender;
	return hr;
}

static int D3D_needreset (void)
{
	HRESULT hr;
	bool do_dd = false;

	if (!devicelost)
		return -1;
	if (d3dex)
		hr = d3ddevex->CheckDeviceState (d3dhwnd);
	else
		hr = d3ddev->TestCooperativeLevel ();
	if (hr == S_PRESENT_OCCLUDED) {
		// no need to draw anything
		return 1;
	}
	if (hr == D3DERR_DEVICELOST) {
		renderdisabled = true;
		// lost but can't be reset yet
		return 1;
	}
	if (hr == D3DERR_DEVICENOTRESET) {
		// lost and can be reset
		write_log (_T("%s: DEVICENOTRESET\n"), D3DHEAD);
		devicelost = 2;
		invalidatedeviceobjects ();
		freetextures ();
		hr = reset ();
		if (FAILED (hr)) {
			write_log (_T("%s: Reset failed %s\n"), D3DHEAD, D3D_ErrorString (hr));
			resetcount++;
			if (resetcount > 2 || hr == D3DERR_DEVICEHUNG) {
				changed_prefs.gfx_api = 0;
				write_log (_T("%s: Too many failed resets, disabling Direct3D mode\n"), D3DHEAD);
			}
			return 1;
		}
		devicelost = 0;
		write_log (_T("%s: Reset succeeded\n"), D3DHEAD);
		renderdisabled = false;
		restoredeviceobjects ();
		alloctextures ();
		return -1;
	} else if (hr == S_PRESENT_MODE_CHANGED) {
		write_log (_T("%s: S_PRESENT_MODE_CHANGED (%d,%d)\n"), D3DHEAD, ddraw_fs, ddraw_fs_attempt);
#if 0
		if (!ddraw_fs) {
			ddraw_fs_attempt++;
			if (ddraw_fs_attempt >= 5) {
				do_dd = true;
			}
		}
#endif
	} 
	if (SUCCEEDED (hr)) {
		devicelost = 0;
		invalidatedeviceobjects ();
		if (do_dd) {
			write_log (_T("%s: S_PRESENT_MODE_CHANGED, Retrying fullscreen with DirectDraw\n"), D3DHEAD);
			ddraw_fs_hack_init ();
		}
		hr = reset ();
		if (FAILED (hr))
			write_log (_T("%s: Reset failed %s\n"), D3DHEAD, D3D_ErrorString (hr));
		restoredeviceobjects ();
		return -1;
	}
	write_log (_T("%s: TestCooperativeLevel %s\n"), D3DHEAD, D3D_ErrorString (hr));
	return 0;
}

static void D3D_showframe2 (bool dowait)
{
	HRESULT hr;

	if (!isd3d ())
		return;
	for (;;) {
		if (d3dswapchain)
			hr = d3dswapchain->Present (NULL, NULL, NULL, NULL, dowait ? 0 : D3DPRESENT_DONOTWAIT);
		else
			hr = d3ddev->Present (NULL, NULL, NULL, NULL);
		if (hr == D3DERR_WASSTILLDRAWING) {
			wasstilldrawing_broken = false;
			if (!dowait)
				return;
			sleep_millis (1);
			continue;
		} else if (hr == S_PRESENT_OCCLUDED) {
			renderdisabled = true;
		} else if (hr == S_PRESENT_MODE_CHANGED) {
			// In most cases mode actually didn't change but
			// D3D is just being stupid and not accepting
			// all modes that DirectDraw does accept,
			// for example interlaced or EDS_RAWMODE modes!
			//devicelost = 1;
			; //write_log (_T("S_PRESENT_MODE_CHANGED\n"));
		} else if (FAILED (hr)) {
			write_log (_T("%s: Present() %s\n"), D3DHEAD, D3D_ErrorString (hr));
			if (hr == D3DERR_DEVICELOST || hr == S_PRESENT_MODE_CHANGED) {
				devicelost = 1;
				renderdisabled = true;
				write_log (_T("%s: mode changed or fullscreen focus lost\n"), D3DHEAD);
			}
		} else {
			ddraw_fs_attempt = 0;
		}
		return;
	}
}

void D3D_restore (void)
{
	renderdisabled = false;
}

void D3D_clear (void)
{
	int i;
	HRESULT hr;

	if (!isd3d ())
		return;
	for (i = 0; i < 2; i++) {
		hr = d3ddev->Clear (0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, d3ddebug ? 0x80 : 0x00), 0, 0);
		D3D_showframe2 (true);		
	}
}

static void D3D_render2 (void)
{
	HRESULT hr;
	LPDIRECT3DTEXTURE9 srctex = texture;
	UINT uPasses, uPass;

	if (!isd3d ())
		return;

	hr = d3ddev->Clear (0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, d3ddebug ? 0x80 : 0, 0), 0, 0);

	if (FAILED (hr = d3ddev->BeginScene ())) {
		write_log (_T("%s: BeginScene: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return;
	}
	if (shaderon > 0 && postEffect) {
		if (psActive) {
			LPDIRECT3DSURFACE9 lpRenderTarget;
			LPDIRECT3DSURFACE9 lpNewRenderTarget;
			LPDIRECT3DTEXTURE9 lpWorkTexture;

			settransform ();
			if (!psEffect_SetTextures (texture, lpWorkTexture1[0], lpWorkTexture2[0], lpHq2xLookupTexture))
				return;
			if (psPreProcess) {
				if (!psEffect_SetMatrices (&m_matPreProj, &m_matPreView, &m_matPreWorld))
					return;

				if (FAILED (hr = d3ddev->GetRenderTarget (0, &lpRenderTarget)))
					write_log (_T("%s: GetRenderTarget: %s\n"), D3DHEAD, D3D_ErrorString (hr));
				lpWorkTexture = lpWorkTexture1[0];
				lpNewRenderTarget = NULL;
	pass2:
				if (FAILED (hr = lpWorkTexture->GetSurfaceLevel (0, &lpNewRenderTarget)))
					write_log (_T("%s: GetSurfaceLevel: %s\n"), D3DHEAD, D3D_ErrorString (hr));
				if (FAILED (hr = d3ddev->SetRenderTarget (0, lpNewRenderTarget)))
					write_log (_T("%s: SetRenderTarget: %s\n"), D3DHEAD, D3D_ErrorString (hr));

				uPasses = 0;
				if (psEffect_Begin (pEffect, (lpWorkTexture == lpWorkTexture1[0]) ? psEffect_PreProcess1 : psEffect_PreProcess2, &uPasses)) {
					for (uPass = 0; uPass < uPasses; uPass++) {
						if (psEffect_BeginPass (pEffect, uPass)) {
							if (FAILED (hr = d3ddev->DrawPrimitive (D3DPT_TRIANGLESTRIP, 4, 2))) {
								write_log (_T("%s: Effect DrawPrimitive failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
							}
							psEffect_EndPass (pEffect);
						}
					}
					psEffect_End (pEffect);
				}
				if (FAILED (hr = d3ddev->SetRenderTarget (0, lpRenderTarget)))
					write_log (_T("%s: Effect RenderTarget reset failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
				lpNewRenderTarget->Release ();
				lpNewRenderTarget = NULL;
				if (psEffect_hasPreProcess2 () && lpWorkTexture == lpWorkTexture1[0]) {
					lpWorkTexture = lpWorkTexture2[0];
					goto pass2;
				}
				lpRenderTarget->Release ();
				lpRenderTarget = NULL;
			}
			psEffect_SetMatrices (&m_matProj2, &m_matView2, &m_matWorld2);

#if TWOPASS
			if (FAILED (hr = d3ddev->GetRenderTarget (0, &lpRenderTarget)))
				write_log (_T("%s: GetRenderTarget: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			if (FAILED (hr = lpTempTexture->GetSurfaceLevel (0, &lpNewRenderTarget)))
				write_log (_T("%s: GetSurfaceLevel: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			if (FAILED (hr = d3ddev->SetRenderTarget (0, lpNewRenderTarget)))
				write_log (_T("%s: SetRenderTarget: %s\n"), D3DHEAD, D3D_ErrorString (hr));
#endif
			hr = d3ddev->Clear (0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, d3ddebug ? 0x80 : 0), 0, 0);

			uPasses = 0;
			if (psEffect_Begin (pEffect, psEffect_Combine, &uPasses)) {
				for (uPass = 0; uPass < uPasses; uPass++) {
					if (!psEffect_BeginPass (pEffect, uPass))
						return;
					if (FAILED (hr = d3ddev->DrawPrimitive (D3DPT_TRIANGLESTRIP, 0, 2)))
						write_log (_T("%s: Effect2 DrawPrimitive failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
					psEffect_EndPass (pEffect);
				}
				psEffect_End (pEffect);
			}
#if TWOPASS
			if (FAILED (hr = d3ddev->SetRenderTarget (0, lpRenderTarget)))
				write_log (_T("%s: SetRenderTarget: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			lpNewRenderTarget->Release ();
			lpRenderTarget->Release ();
#endif
			srctex = lpTempTexture;

		}

	}

#if TWOPASS
	if (shaderon > 0 && postEffect) {

		if (masktexture) {
			if (FAILED (hr = postEffect->SetTechnique (postTechnique)))
				write_log (_T("%s: SetTechnique(postTechnique) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			if (FAILED (hr = postEffect->SetTexture (postMaskTextureHandle, masktexture)))
				write_log (_T("%s: SetTexture(masktexture) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		} else if (sltexture) {
			if (FAILED (hr = postEffect->SetTechnique (postTechniqueAlpha)))
				write_log (_T("%s: SetTechnique(postTechniqueAlpha) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			if (FAILED (hr = postEffect->SetTexture (postMaskTextureHandle, sltexture)))
				write_log (_T("%s: SetTexture(sltexture) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		} else {
			if (FAILED (hr = postEffect->SetTechnique (postTechniquePlain)))
				write_log (_T("%s: SetTechnique(postTechniquePlain) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		}
		hr = postEffect->SetInt (postFilterMode, currprefs.gfx_filter_bilinear ? D3DTEXF_LINEAR : D3DTEXF_POINT);

		if (FAILED (hr = postEffect->SetTexture (postSourceTextureHandle, srctex)))
			write_log (_T("%s: SetTexture(srctex) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		setupscenecoords ();
		hr = postEffect->SetMatrix (postMatrixSource, &postproj);
		hr = postEffect->SetVector (postMaskMult, &maskmult);
		hr = postEffect->SetVector (postMaskShift, &maskshift);
		hr = postEffect->SetVector (postTexelSize, &texelsize);

		uPasses = 0;
		if (psEffect_Begin (postEffect, psEffect_None, &uPasses)) {
			for (uPass = 0; uPass < uPasses; uPass++) {
				if (psEffect_BeginPass (postEffect, uPass)) {
					if (FAILED (hr = d3ddev->DrawPrimitive (D3DPT_TRIANGLESTRIP, 0, 2)))
						write_log (_T("%s: Post DrawPrimitive failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
					psEffect_EndPass (postEffect);
				}
			}
			psEffect_End (postEffect);
		}

	} else {

		// non-shader version
		setupscenecoords ();
		hr = d3ddev->SetTransform (D3DTS_PROJECTION, &m_matProj);
		hr = d3ddev->SetTransform (D3DTS_VIEW, &m_matView);
		hr = d3ddev->SetTransform (D3DTS_WORLD, &m_matWorld);
		hr = d3ddev->SetTexture (0, srctex);
		hr = d3ddev->DrawPrimitive (D3DPT_TRIANGLESTRIP, 0, 2);
		int bl = currprefs.gfx_filter_bilinear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		hr = d3ddev->SetSamplerState (0, D3DSAMP_MINFILTER, bl);
		hr = d3ddev->SetSamplerState (0, D3DSAMP_MAGFILTER, bl);

		if (sprite && sltexture) {
			D3DXVECTOR3 v;
			sprite->Begin (D3DXSPRITE_ALPHABLEND);
			v.x = v.y = v.z = 0;
			sprite->Draw (sltexture, NULL, NULL, &v, 0xffffffff);
			sprite->End ();
		}
	}

	if (sprite && ((ledtexture) || (mask2texture) || (cursorsurfaced3d && cursor_v))) {
		D3DXVECTOR3 v;
		sprite->Begin (D3DXSPRITE_ALPHABLEND);
		if (cursorsurfaced3d && cursor_v) {
			D3DXMATRIXA16 t;

			MatrixScaling (&t, ((float)(window_w) / (tout_w + 2 * cursor_offset2_x)), ((float)(window_h) / (tout_h + 2 * cursor_offset2_y)), 0);
			v.x = cursor_x + cursor_offset2_x;
			v.y = cursor_y + cursor_offset2_y;
			v.z = 0;
			sprite->SetTransform (&t);
			sprite->Draw (cursorsurfaced3d, NULL, NULL, &v, 0xffffffff);
			MatrixScaling (&t, 1, 1, 0);
			sprite->Flush ();
			sprite->SetTransform (&t);
		}
		if (mask2texture) {
			D3DXMATRIXA16 t;
			RECT r;
			float srcw = mask2texture_w;
			float srch = mask2texture_h;
			float aspectsrc = srcw / srch;
			float aspectdst = (float)window_w / window_h;
			float w, h;

			w = mask2texture_multx;
			h = mask2texture_multy;
#if 0
			if (currprefs.gfx_filteroverlay_pos.width > 0)
				w = (float)currprefs.gfx_filteroverlay_pos.width / srcw;
			else if (currprefs.gfx_filteroverlay_pos.width == -1)
				w = 1.0;
			else if (currprefs.gfx_filteroverlay_pos.width <= -24000)
				w = w * (-currprefs.gfx_filteroverlay_pos.width - 30000) / 100.0;

			if (currprefs.gfx_filteroverlay_pos.height > 0)
				h = (float)currprefs.gfx_filteroverlay_pos.height / srch;
			else if (currprefs.gfx_filteroverlay_pos.height == -1)
				h = 1;
			else if (currprefs.gfx_filteroverlay_pos.height <= -24000)
				h = h * (-currprefs.gfx_filteroverlay_pos.height - 30000) / 100.0;
#endif
			MatrixScaling (&t, w, h, 0);

			v.x = 0;
			if (currprefs.gfx_filteroverlay_pos.x == -1)
				v.x = (window_w - (mask2texture_w * w)) / 2;
			else if (currprefs.gfx_filteroverlay_pos.x > -24000)
				v.x = currprefs.gfx_filteroverlay_pos.x;
			else
				v.x = (window_w - (mask2texture_w * w)) / 2 + (-currprefs.gfx_filteroverlay_pos.x - 30100) * window_w / 100.0;

			v.y = 0;
			if (currprefs.gfx_filteroverlay_pos.y == -1)
				v.y = (window_h - (mask2texture_h * h)) / 2;
			else if (currprefs.gfx_filteroverlay_pos.y > -24000)
				v.y = currprefs.gfx_filteroverlay_pos.y;
			else
				v.y = (window_h - (mask2texture_h * h)) / 2 + (-currprefs.gfx_filteroverlay_pos.y - 30100) * window_h / 100.0;

			v.x /= w;
			v.y /= h;
			v.x = v.y = 0;
			v.z = 0;
			v.x += mask2texture_offsetw / w;

			r.left = 0;
			r.top = 0;
			r.right = mask2texture_w;
			r.bottom = mask2texture_h;
			if (showoverlay) {
				sprite->SetTransform (&t);
				sprite->Draw (mask2texture, &r, NULL, &v, 0xffffffff);
				sprite->Flush ();
			}
			MatrixScaling (&t, 1, 1, 0);
			sprite->SetTransform (&t);

			if (mask2texture_offsetw > 0) {
				v.x = 0;
				v.y = 0;
				r.left = 0;
				r.top = 0;
				r.right = mask2texture_offsetw + 1;
				r.bottom = window_h;
				sprite->Draw (blanktexture, &r, NULL, &v, 0xffffffff);
				if (window_w > mask2texture_offsetw + mask2texture_ww) {
					v.x = mask2texture_offsetw + mask2texture_ww;
					v.y = 0;
					r.left = 0;
					r.top = 0;
					r.right = window_w - (mask2texture_offsetw + mask2texture_ww) + 1;
					r.bottom = window_h;
					sprite->Draw (blanktexture, &r, NULL, &v, 0xffffffff);
				}
			}

		}
		if (ledtexture && (((currprefs.leds_on_screen & STATUSLINE_RTG) && WIN32GFX_IsPicassoScreen ()) || ((currprefs.leds_on_screen & STATUSLINE_CHIPSET) && !WIN32GFX_IsPicassoScreen ()))) {
			int slx, sly;
			statusline_getpos (&slx, &sly, window_w, window_h);
			v.x = slx;
			v.y = sly;
			v.z = 0;
			sprite->Draw (ledtexture, NULL, NULL, &v, 0xffffffff);
		}
		sprite->End ();
	}
#endif

	hr = d3ddev->EndScene ();
	if (FAILED (hr))
		write_log (_T("%s: EndScene() %s\n"), D3DHEAD, D3D_ErrorString (hr));
}

void D3D_setcursor (int x, int y, int width, int height, bool visible)
{
	if (width && height) {
		cursor_offset2_x = cursor_offset_x * window_w / width;
		cursor_offset2_y = cursor_offset_y * window_h / height;
		cursor_x = x * window_w / width;
		cursor_y = y * window_h / height;
	} else {
		cursor_x = cursor_y = 0;
		cursor_offset2_x = cursor_offset2_y = 0;
	}
	cursor_v = visible;
}

void D3D_unlocktexture (void)
{
	HRESULT hr;

	if (!isd3d () || !texture)
		return;

	if (locked) {
		if (currprefs.leds_on_screen & (STATUSLINE_CHIPSET | STATUSLINE_RTG))
			updateleds ();
		hr = texture->UnlockRect (0);
	}
	locked = 0;
	fulllocked = 0;
}

void D3D_flushtexture (int miny, int maxy)
{
	if (fakemode || fulllocked || !texture || renderdisabled)
		return;
	if (miny >= 0 && maxy >= 0) {
		RECT r;
		maxy++;
		r.left = 0;
		r.right = tin_w;
		r.top = miny <= 0 ? 0 : miny;
		r.bottom = maxy <= tin_h ? maxy : tin_h;
		if (r.top <= r.bottom) {
			HRESULT hr = texture->AddDirtyRect (&r);
			if (FAILED (hr))
				write_log (_T("%s: AddDirtyRect(): %s\n"), D3DHEAD, D3D_ErrorString (hr));
			//write_log (_T("%d %d\n"), r.top, r.bottom);
		}
	}
}

uae_u8 *D3D_locktexture (int *pitch, bool fullupdate)
{
	D3DLOCKED_RECT lock;
	HRESULT hr;

	if (fakemode) {
		*pitch = 0;
		return fakebitmap;
	}

	if (D3D_needreset () > 0) {
		return NULL;
	}
	if (!isd3d () || !texture)
		return NULL;

	if (locked) {
		write_log (_T("%s: texture already locked!\n"), D3DHEAD);
		return NULL;
	}

	lock.pBits = NULL;
	lock.Pitch = 0;
	hr = texture->LockRect (0, &lock, NULL, fullupdate ? D3DLOCK_DISCARD : D3DLOCK_NO_DIRTY_UPDATE);
	if (FAILED (hr)) {
		write_log (_T("%s: LockRect failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
		return NULL;
	}
	if (lock.pBits == NULL || lock.Pitch == 0) {
		write_log (_T("%s: LockRect returned NULL texture\n"), D3DHEAD);
		D3D_unlocktexture ();
		return NULL;
	}
	locked = 1;
	fulllocked = fullupdate;
	*pitch = lock.Pitch;
	return (uae_u8*)lock.pBits;
}

static void flushgpu (bool wait)
{
	if (currprefs.turbo_emulation)
		return;
	if (query) {
		HRESULT hr = query->Issue (D3DISSUE_END);
		if (SUCCEEDED (hr)) {
			while (query->GetData (NULL, 0, D3DGETDATA_FLUSH) == S_FALSE) {
				if (!wait)
					return;
			}
		} else {
			static int reported;
			if (reported < 10) {
				reported++;
				write_log (_T("%s: query->Issue (D3DISSUE_END) failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			}
		}
	}
}

bool D3D_renderframe (bool immediate)
{
	static int vsync2_cnt;

	if (fakemode)
		return true;

	if (!isd3d () || !texture)
		return false;

	if (filenotificationhandle != NULL) {
		bool notify = false;
		while (WaitForSingleObject (filenotificationhandle, 0) == WAIT_OBJECT_0) {
			FindNextChangeNotification (filenotificationhandle);
			notify = true;
		}
		if (notify) {
			devicelost = 2;
			write_log (_T("%s: Shader file modification notification\n"), D3DHEAD);
		}
	}

	if (vsync2 > 0) {
		vsync2_cnt ^= 1;
		if (vsync2_cnt == 0)
			return true;
	}

	D3D_render2 ();
	flushgpu (immediate);

	return true;
}

void D3D_showframe (void)
{
	if (!isd3d ())
		return;
	if (currprefs.turbo_emulation) {
		if (!(dpp.PresentationInterval & D3DPRESENT_INTERVAL_IMMEDIATE) && wasstilldrawing_broken) {
			static int frameskip;
			if (currprefs.turbo_emulation && frameskip-- > 0)
				return;
			frameskip = 50;
		}
		D3D_showframe2 (false);
	} else {
		D3D_showframe2 (true);
		if (vsync2 < 0 && !currprefs.turbo_emulation) {
			D3D_showframe2 (true);
		}
	}
	flushgpu (true);
}

void D3D_refresh (void)
{
	if (!isd3d ())
		return;
	D3D_render2 ();
	D3D_showframe2 (true);
	D3D_render2 ();
	D3D_showframe2 (true);
	createscanlines (0);
}

void D3D_getpixelformat (int depth, int *rb, int *gb, int *bb, int *rs, int *gs, int *bs, int *ab, int *as, int *a)
{
	switch (depth)
	{
	case 32:
		*rb = 8;
		*gb = 8;
		*bb = 8;
		*ab = 8;
		*rs = 16;
		*gs = 8;
		*bs = 0;
		*as = 24;
		*a = 0;
		break;
	case 15:
		*rb = 5;
		*gb = 5;
		*bb = 5;
		*ab = 1;
		*rs = 10;
		*gs = 5;
		*bs = 0;
		*as = 15;
		*a = 0;
	break;
	case 16:
		*rb = 5;
		*gb = 6;
		*bb = 5;
		*ab = 0;
		*rs = 11;
		*gs = 5;
		*bs = 0;
		*as = 0;
		*a = 0;
		break;
	}
}

double D3D_getrefreshrate (void)
{
	HRESULT hr;
	D3DDISPLAYMODE dmode;

	if (!isd3d ())
		return -1;
	hr = d3ddev->GetDisplayMode (0, &dmode);
	if (FAILED (hr))
		return -1;
	return dmode.RefreshRate;
}

void D3D_guimode (bool guion)
{
	HRESULT hr;

	if (!isd3d ())
		return;
	hr = d3ddev->SetDialogBoxMode (guion ? TRUE : FALSE);
	if (FAILED (hr))
		write_log (_T("%s: SetDialogBoxMode %s\n"), D3DHEAD, D3D_ErrorString (hr));
	guimode = guion;
}

HDC D3D_getDC (HDC hdc)
{
	static LPDIRECT3DSURFACE9 bb;
	HRESULT hr;

	if (!isd3d ())
		return 0;
	if (!hdc) {
		hr = d3ddev->GetBackBuffer (0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
		if (FAILED (hr)) {
			write_log (_T("%s: GetBackBuffer() failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
			return 0;
		}
		hr = bb->GetDC (&hdc);
		if (SUCCEEDED (hr))
			return hdc;
		write_log (_T("%s: GetDC() failed: %s\n"), D3DHEAD, D3D_ErrorString (hr));
	}
	if (hdc)
		bb->ReleaseDC (hdc);
	bb->Release ();
	bb = NULL;
	return 0;
}

int D3D_isenabled (void)
{
	return d3d_enabled;
}

#endif
