#include "Log.hpp"
#include "RuntimeD3D11.hpp"
#include "EffectParserTree.hpp"

#include <d3dcompiler.h>
#include <nanovg_d3d11.h>
#include <boost\algorithm\string.hpp>
#include <boost\filesystem.hpp>
#include <stb_dxt.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <stb_image_resize.h>

// -----------------------------------------------------------------------------------------------------
extern int BackbufferWidth;
extern int BackbufferHeight;

std::vector<ID3D11RenderTargetView*> ReShade::Runtimes::D3D11Runtime::WatchedRTs;

bool ReShade::Runtimes::D3D11Runtime::mIsDumpingTrace;
int ReShade::Runtimes::D3D11Runtime::mToggleDebugViewID;

int CurrentSavedView = -1;
std::vector<int> SavedViews;


inline bool operator ==(const D3D11_SAMPLER_DESC &left, const D3D11_SAMPLER_DESC &right)
{
	return std::memcmp(&left, &right, sizeof(D3D11_SAMPLER_DESC)) == 0;
}

namespace ReShade { namespace Runtimes
{
	namespace
	{
		struct CSLock : boost::noncopyable
		{
			CSLock(CRITICAL_SECTION &cs) : mCS(cs)
			{
				EnterCriticalSection(&this->mCS);
			}
			~CSLock()
			{
				LeaveCriticalSection(&this->mCS);
			}

			CRITICAL_SECTION &mCS;
		};

		class D3D11EffectCompiler : private boost::noncopyable
		{
		public:
			D3D11EffectCompiler(const EffectTree &ast) : mAST(ast), mEffect(nullptr), mCurrentInParameterBlock(false), mCurrentInFunctionBlock(false), mCurrentInDeclaratorList(false), mCurrentGlobalSize(0), mCurrentGlobalStorageSize(0), mFatal(false)
			{
			}

			bool Traverse(D3D11Effect *effect, std::string &errors)
			{
				this->mEffect = effect;
				this->mErrors.clear();
				this->mFatal = false;
				this->mCurrentSource.clear();

				// Global constant buffer
				this->mEffect->mConstantBuffers.push_back(nullptr);
				this->mEffect->mConstantStorages.push_back(nullptr);

				const EffectNodes::Root *node = &this->mAST[EffectTree::Root].As<EffectNodes::Root>();

				do
				{
					Visit(*node);

					if (node->NextDeclaration != EffectTree::Null)
					{
						node = &this->mAST[node->NextDeclaration].As<EffectNodes::Root>();
					}
					else
					{
						node = nullptr;
					}
				}
				while (node != nullptr);

				if (this->mCurrentGlobalSize != 0)
				{
					CD3D11_BUFFER_DESC globalsDesc(RoundToMultipleOf16(this->mCurrentGlobalSize), D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
					D3D11_SUBRESOURCE_DATA globalsInitial;
					globalsInitial.pSysMem = this->mEffect->mConstantStorages[0];
					globalsInitial.SysMemPitch = globalsInitial.SysMemSlicePitch = this->mCurrentGlobalSize;
					this->mEffect->mRuntime->mDevice->CreateBuffer(&globalsDesc, &globalsInitial, &this->mEffect->mConstantBuffers[0]);
				}

				errors += this->mErrors;

				return !this->mFatal;
			}

			static inline UINT RoundToMultipleOf16(UINT size)
			{
				return (size + 15) & ~15;
			}
			static D3D11_TEXTURE_ADDRESS_MODE LiteralToTextureAddress(int value)
			{
				switch (value)
				{
					default:
					case EffectNodes::Literal::CLAMP:
						return D3D11_TEXTURE_ADDRESS_CLAMP;
					case EffectNodes::Literal::REPEAT:
						return D3D11_TEXTURE_ADDRESS_WRAP;
					case EffectNodes::Literal::MIRROR:
						return D3D11_TEXTURE_ADDRESS_MIRROR;
					case EffectNodes::Literal::BORDER:
						return D3D11_TEXTURE_ADDRESS_BORDER;
				}
			}
			static D3D11_COMPARISON_FUNC LiteralToComparisonFunc(int value)
			{
				const D3D11_COMPARISON_FUNC conversion[] =
				{
					D3D11_COMPARISON_NEVER, D3D11_COMPARISON_ALWAYS, D3D11_COMPARISON_LESS, D3D11_COMPARISON_GREATER, D3D11_COMPARISON_LESS_EQUAL, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_NOT_EQUAL
				};

				const unsigned int conversionIndex = value - EffectNodes::Literal::NEVER;

				assert(conversionIndex < ARRAYSIZE(conversion));

				return conversion[conversionIndex];
			}
			static D3D11_STENCIL_OP LiteralToStencilOp(int value)
			{
				switch (value)
				{
					default:
					case EffectNodes::Literal::KEEP:
						return D3D11_STENCIL_OP_KEEP;
					case EffectNodes::Literal::ZERO:
						return D3D11_STENCIL_OP_ZERO;
					case EffectNodes::Literal::REPLACE:
						return D3D11_STENCIL_OP_REPLACE;
					case EffectNodes::Literal::INCR:
						return D3D11_STENCIL_OP_INCR;
					case EffectNodes::Literal::INCRSAT:
						return D3D11_STENCIL_OP_INCR_SAT;
					case EffectNodes::Literal::DECR:
						return D3D11_STENCIL_OP_DECR;
					case EffectNodes::Literal::DECRSAT:
						return D3D11_STENCIL_OP_DECR_SAT;
					case EffectNodes::Literal::INVERT:
						return D3D11_STENCIL_OP_INVERT;
				}
			}
			static D3D11_BLEND LiteralToBlend(int value)
			{
				switch (value)
				{
					default:
					case EffectNodes::Literal::ZERO:
						return D3D11_BLEND_ZERO;
					case EffectNodes::Literal::ONE:
						return D3D11_BLEND_ONE;
					case EffectNodes::Literal::SRCCOLOR:
						return D3D11_BLEND_SRC_COLOR;
					case EffectNodes::Literal::SRCALPHA:
						return D3D11_BLEND_SRC_ALPHA;
					case EffectNodes::Literal::INVSRCCOLOR:
						return D3D11_BLEND_INV_SRC_COLOR;
					case EffectNodes::Literal::INVSRCALPHA:
						return D3D11_BLEND_INV_SRC_ALPHA;
					case EffectNodes::Literal::DESTCOLOR:
						return D3D11_BLEND_DEST_COLOR;
					case EffectNodes::Literal::DESTALPHA:
						return D3D11_BLEND_DEST_ALPHA;
					case EffectNodes::Literal::INVDESTCOLOR:
						return D3D11_BLEND_INV_DEST_COLOR;
					case EffectNodes::Literal::INVDESTALPHA:
						return D3D11_BLEND_INV_DEST_ALPHA;
				}
			}
			static D3D11_BLEND_OP LiteralToBlendOp(int value)
			{
				switch (value)
				{
					default:
					case EffectNodes::Literal::ADD:
						return D3D11_BLEND_OP_ADD;
					case EffectNodes::Literal::SUBTRACT:
						return D3D11_BLEND_OP_SUBTRACT;
					case EffectNodes::Literal::REVSUBTRACT:
						return D3D11_BLEND_OP_REV_SUBTRACT;
					case EffectNodes::Literal::MIN:
						return D3D11_BLEND_OP_MIN;
					case EffectNodes::Literal::MAX:
						return D3D11_BLEND_OP_MAX;
				}
			}
			static DXGI_FORMAT LiteralToFormat(int value, Effect::Texture::Format &format)
			{
				switch (value)
				{
					case EffectNodes::Literal::R8:
						format = Effect::Texture::Format::R8;
						return DXGI_FORMAT_R8_UNORM;
					case EffectNodes::Literal::R32F:
						format = Effect::Texture::Format::R32F;
						return DXGI_FORMAT_R32_FLOAT;
					case EffectNodes::Literal::RG8:
						format = Effect::Texture::Format::RG8;
						return DXGI_FORMAT_R8G8_UNORM;
					case EffectNodes::Literal::RGBA8:
						format = Effect::Texture::Format::RGBA8;
						return DXGI_FORMAT_R8G8B8A8_TYPELESS;
					case EffectNodes::Literal::RGBA16:
						format = Effect::Texture::Format::RGBA16;
						return DXGI_FORMAT_R16G16B16A16_UNORM;
					case EffectNodes::Literal::RGBA16F:
						format = Effect::Texture::Format::RGBA16F;
						return DXGI_FORMAT_R16G16B16A16_FLOAT;
					case EffectNodes::Literal::RGBA32F:
						format = Effect::Texture::Format::RGBA32F;
						return DXGI_FORMAT_R32G32B32A32_FLOAT;
					case EffectNodes::Literal::DXT1:
						format = Effect::Texture::Format::DXT1;
						return DXGI_FORMAT_BC1_TYPELESS;
					case EffectNodes::Literal::DXT3:
						format = Effect::Texture::Format::DXT3;
						return DXGI_FORMAT_BC2_TYPELESS;
					case EffectNodes::Literal::DXT5:
						format = Effect::Texture::Format::DXT5;
						return DXGI_FORMAT_BC3_TYPELESS;
					case EffectNodes::Literal::LATC1:
						format = Effect::Texture::Format::LATC1;
						return DXGI_FORMAT_BC4_UNORM;
					case EffectNodes::Literal::LATC2:
						format = Effect::Texture::Format::LATC2;
						return DXGI_FORMAT_BC5_UNORM;
					default:
						format = Effect::Texture::Format::Unknown;
						return DXGI_FORMAT_UNKNOWN;
				}
			}
			static DXGI_FORMAT MakeTypelessFormat(DXGI_FORMAT format)
			{
				switch (format)
				{
					case DXGI_FORMAT_R8G8B8A8_UNORM:
					case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
						return DXGI_FORMAT_R8G8B8A8_TYPELESS;
					case DXGI_FORMAT_BC1_UNORM:
					case DXGI_FORMAT_BC1_UNORM_SRGB:
						return DXGI_FORMAT_BC1_TYPELESS;
					case DXGI_FORMAT_BC2_UNORM:
					case DXGI_FORMAT_BC2_UNORM_SRGB:
						return DXGI_FORMAT_BC2_TYPELESS;
					case DXGI_FORMAT_BC3_UNORM:
					case DXGI_FORMAT_BC3_UNORM_SRGB:
						return DXGI_FORMAT_BC3_TYPELESS;
					default:
						return format;
				}
			}
			static DXGI_FORMAT MakeSRGBFormat(DXGI_FORMAT format)
			{
				switch (format)
				{
					case DXGI_FORMAT_R8G8B8A8_TYPELESS:
					case DXGI_FORMAT_R8G8B8A8_UNORM:
						return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
					case DXGI_FORMAT_BC1_TYPELESS:
					case DXGI_FORMAT_BC1_UNORM:
						return DXGI_FORMAT_BC1_UNORM_SRGB;
					case DXGI_FORMAT_BC2_TYPELESS:
					case DXGI_FORMAT_BC2_UNORM:
						return DXGI_FORMAT_BC2_UNORM_SRGB;
					case DXGI_FORMAT_BC3_TYPELESS:
					case DXGI_FORMAT_BC3_UNORM:
						return DXGI_FORMAT_BC3_UNORM_SRGB;
					default:
						return format;
				}
			}
			static DXGI_FORMAT MakeNonSRBFormat(DXGI_FORMAT format)
			{
				switch (format)
				{
					case DXGI_FORMAT_R8G8B8A8_TYPELESS:
					case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
						return DXGI_FORMAT_R8G8B8A8_UNORM;
					case DXGI_FORMAT_BC1_TYPELESS:
					case DXGI_FORMAT_BC1_UNORM_SRGB:
						return DXGI_FORMAT_BC1_UNORM;
					case DXGI_FORMAT_BC2_TYPELESS:
					case DXGI_FORMAT_BC2_UNORM_SRGB:
						return DXGI_FORMAT_BC2_UNORM;
					case DXGI_FORMAT_BC3_TYPELESS:
					case DXGI_FORMAT_BC3_UNORM_SRGB:
						return DXGI_FORMAT_BC3_UNORM;
					default:
						return format;
				}
			}
			static std::size_t D3D11_SAMPLER_DESC_HASH(const D3D11_SAMPLER_DESC &s) 
			{
				const unsigned char *p = reinterpret_cast<const unsigned char *>(&s);
				std::size_t h = 2166136261;

				for (std::size_t i = 0; i < sizeof(D3D11_SAMPLER_DESC); ++i)
				{
					h = (h * 16777619) ^ p[i];
				}

				return h;
			}

			static std::string ConvertSemantic(const std::string &semantic)
			{
				if (semantic == "VERTEXID")
				{
					return "SV_VERTEXID";
				}
				else if (semantic == "POSITION" || semantic == "VPOS")
				{
					return "SV_POSITION";
				}
				else if (boost::starts_with(semantic, "COLOR"))
				{
					return "SV_TARGET" + semantic.substr(5);
				}
				else if (semantic == "DEPTH")
				{
					return "SV_DEPTH";
				}

				return semantic;
			}
			static inline std::string PrintLocation(const EffectTree::Location &location)
			{
				return std::string(location.Source != nullptr ? location.Source : "") + "(" + std::to_string(location.Line) + ", " + std::to_string(location.Column) + "): ";
			}
			std::string PrintType(const EffectNodes::Type &type)
			{
				std::string res;

				switch (type.Class)
				{
					case EffectNodes::Type::Void:
						res += "void";
						break;
					case EffectNodes::Type::Bool:
						res += "bool";
						break;
					case EffectNodes::Type::Int:
						res += "int";
						break;
					case EffectNodes::Type::Uint:
						res += "uint";
						break;
					case EffectNodes::Type::Float:
						res += "float";
						break;
					case EffectNodes::Type::Sampler:
						res += "__sampler2D";
						break;
					case EffectNodes::Type::Struct:
						assert(type.Definition != EffectTree::Null);
						res += this->mAST[type.Definition].As<EffectNodes::Struct>().Name;
						break;
				}

				if (type.IsMatrix())
				{
					res += std::to_string(type.Rows) + "x" + std::to_string(type.Cols);
				}
				else if (type.IsVector())
				{
					res += std::to_string(type.Rows);
				}

				return res;
			}
			std::string PrintTypeWithQualifiers(const EffectNodes::Type &type)
			{
				std::string qualifiers;

				if (type.HasQualifier(EffectNodes::Type::Qualifier::Extern))
					qualifiers += "extern ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Static))
					qualifiers += "static ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Const))
					qualifiers += "const ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Volatile))
					qualifiers += "volatile ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Precise))
					qualifiers += "precise ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::NoInterpolation))
					qualifiers += "nointerpolation ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::NoPerspective))
					qualifiers += "noperspective ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Linear))
					qualifiers += "linear ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Centroid))
					qualifiers += "centroid ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::Sample))
					qualifiers += "sample ";
				if (type.HasQualifier(EffectNodes::Type::Qualifier::InOut))
					qualifiers += "inout ";
				else if (type.HasQualifier(EffectNodes::Type::Qualifier::In))
					qualifiers += "in ";
				else if (type.HasQualifier(EffectNodes::Type::Qualifier::Out))
					qualifiers += "out ";
				else if (type.HasQualifier(EffectNodes::Type::Qualifier::Uniform))
					qualifiers += "uniform ";

				return qualifiers + PrintType(type);
			}

			void Visit(const EffectTree::Node &node)
			{
				using namespace EffectNodes;

				if (node.Is<Variable>())
				{
					Visit(node.As<Variable>());
				}
				else if (node.Is<Function>())
				{
					Visit(node.As<Function>());
				}
				else if (node.Is<Literal>())
				{
					Visit(node.As<Literal>());
				}
				else if (node.Is<LValue>())
				{
					Visit(node.As<LValue>());
				}
				else if (node.Is<Expression>())
				{
					Visit(node.As<Expression>());
				}
				else if (node.Is<Swizzle>())
				{
					Visit(node.As<Swizzle>());
				}
				else if (node.Is<Assignment>())
				{
					Visit(node.As<Assignment>());
				}
				else if (node.Is<Sequence>())
				{
					Visit(node.As<Sequence>());
				}
				else if (node.Is<Constructor>())
				{
					Visit(node.As<Constructor>());
				}
				else if (node.Is<Call>())
				{
					Visit(node.As<Call>());
				}
				else if (node.Is<InitializerList>())
				{
					Visit(node.As<InitializerList>());
				}
				else if (node.Is<ExpressionStatement>())
				{
					Visit(node.As<ExpressionStatement>());
				}
				else if (node.Is<DeclarationStatement>())
				{
					Visit(node.As<DeclarationStatement>());
				}
				else if (node.Is<If>())
				{
					Visit(node.As<If>());
				}
				else if (node.Is<Switch>())
				{
					Visit(node.As<Switch>());
				}
				else if (node.Is<For>())
				{
					Visit(node.As<For>());
				}
				else if (node.Is<While>())
				{
					Visit(node.As<While>());
				}
				else if (node.Is<Return>())
				{
					Visit(node.As<Return>());
				}
				else if (node.Is<Jump>())
				{
					Visit(node.As<Jump>());
				}
				else if (node.Is<StatementBlock>())
				{
					Visit(node.As<StatementBlock>());
				}
				else if (node.Is<Struct>())
				{
					Visit(node.As<Struct>());
				}
				else if (node.Is<Technique>())
				{
					Visit(node.As<Technique>());
				}
				else if (!node.Is<Root>())
				{
					assert(false);
				}
			}
			void Visit(const EffectNodes::LValue &node)
			{
				this->mCurrentSource += this->mAST[node.Reference].As<EffectNodes::Variable>().Name;
			}
			void Visit(const EffectNodes::Literal &node)
			{
				if (!node.Type.IsScalar())
				{
					this->mCurrentSource += PrintType(node.Type);
					this->mCurrentSource += '(';
				}

				for (unsigned int i = 0; i < node.Type.Rows * node.Type.Cols; ++i)
				{
					switch (node.Type.Class)
					{
						case EffectNodes::Type::Bool:
							this->mCurrentSource += node.Value.Bool[i] ? "true" : "false";
							break;
						case EffectNodes::Type::Int:
							this->mCurrentSource += std::to_string(node.Value.Int[i]);
							break;
						case EffectNodes::Type::Uint:
							this->mCurrentSource += std::to_string(node.Value.Uint[i]);
							break;
						case EffectNodes::Type::Float:
							this->mCurrentSource += std::to_string(node.Value.Float[i]) + "f";
							break;
					}

					this->mCurrentSource += ", ";
				}

				this->mCurrentSource.pop_back();
				this->mCurrentSource.pop_back();

				if (!node.Type.IsScalar())
				{
					this->mCurrentSource += ')';
				}
			}
			void Visit(const EffectNodes::Expression &node)
			{
				std::string part1, part2, part3, part4;

				switch (node.Operator)
				{
					case EffectNodes::Expression::Negate:
						part1 = '-';
						break;
					case EffectNodes::Expression::BitNot:
						part1 = '~';
						break;
					case EffectNodes::Expression::LogicNot:
						part1 = '!';
						break;
					case EffectNodes::Expression::Increase:
						part1 = "++";
						break;
					case EffectNodes::Expression::Decrease:
						part1 = "--";
						break;
					case EffectNodes::Expression::PostIncrease:
						part2 = "++";
						break;
					case EffectNodes::Expression::PostDecrease:
						part2 = "--";
						break;
					case EffectNodes::Expression::Abs:
						part1 = "abs(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Sign:
						part1 = "sign(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Rcp:
						part1 = "rcp(";
						part2 = ")";
						break;
					case EffectNodes::Expression::All:
						part1 = "all(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Any:
						part1 = "any(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Sin:
						part1 = "sin(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Sinh:
						part1 = "sinh(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Cos:
						part1 = "cos(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Cosh:
						part1 = "cosh(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Tan:
						part1 = "tan(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Tanh:
						part1 = "tanh(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Asin:
						part1 = "asin(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Acos:
						part1 = "acos(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Atan:
						part1 = "atan(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Exp:
						part1 = "exp(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Exp2:
						part1 = "exp2(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Log:
						part1 = "log(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Log2:
						part1 = "log2(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Log10:
						part1 = "log10(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Sqrt:
						part1 = "sqrt(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Rsqrt:
						part1 = "rsqrt(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Ceil:
						part1 = "ceil(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Floor:
						part1 = "floor(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Frac:
						part1 = "frac(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Trunc:
						part1 = "trunc(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Round:
						part1 = "round(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Saturate:
						part1 = "saturate(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Radians:
						part1 = "radians(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Degrees:
						part1 = "degrees(";
						part2 = ")";
						break;
					case EffectNodes::Expression::PartialDerivativeX:
						part1 = "ddx(";
						part2 = ")";
						break;
					case EffectNodes::Expression::PartialDerivativeY:
						part1 = "ddy(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Noise:
						part1 = "noise(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Length:
						part1 = "length(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Normalize:
						part1 = "normalize(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Transpose:
						part1 = "transpose(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Determinant:
						part1 = "determinant(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Cast:
						part1 = PrintType(node.Type) + '(';
						part2 = ')';
						break;
					case EffectNodes::Expression::BitCastInt2Float:
						part1 = "asfloat(";
						part2 = ")";
						break;
					case EffectNodes::Expression::BitCastUint2Float:
						part1 = "asfloat(";
						part2 = ")";
						break;
					case EffectNodes::Expression::BitCastFloat2Int:
						part1 = "asint(";
						part2 = ")";
						break;
					case EffectNodes::Expression::BitCastFloat2Uint:
						part1 = "asuint(";
						part2 = ")";
						break;
					case EffectNodes::Expression::Add:
						part1 = '(';
						part2 = " + ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Subtract:
						part1 = '(';
						part2 = " - ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Multiply:
						part1 = '(';
						part2 = " * ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Divide:
						part1 = '(';
						part2 = " / ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Modulo:
						part1 = '(';
						part2 = " % ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Less:
						part1 = '(';
						part2 = " < ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Greater:
						part1 = '(';
						part2 = " > ";
						part3 = ')';
						break;
					case EffectNodes::Expression::LessOrEqual:
						part1 = '(';
						part2 = " <= ";
						part3 = ')';
						break;
					case EffectNodes::Expression::GreaterOrEqual:
						part1 = '(';
						part2 = " >= ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Equal:
						part1 = '(';
						part2 = " == ";
						part3 = ')';
						break;
					case EffectNodes::Expression::NotEqual:
						part1 = '(';
						part2 = " != ";
						part3 = ')';
						break;
					case EffectNodes::Expression::LeftShift:
						part1 = '(';
						part2 = " << ";
						part3 = ')';
						break;
					case EffectNodes::Expression::RightShift:
						part1 = '(';
						part2 = " >> ";
						part3 = ')';
						break;
					case EffectNodes::Expression::BitAnd:
						part1 = '(';
						part2 = " & ";
						part3 = ')';
						break;
					case EffectNodes::Expression::BitXor:
						part1 = '(';
						part2 = " ^ ";
						part3 = ')';
						break;
					case EffectNodes::Expression::BitOr:
						part1 = '(';
						part2 = " | ";
						part3 = ')';
						break;
					case EffectNodes::Expression::LogicAnd:
						part1 = '(';
						part2 = " && ";
						part3 = ')';
						break;
					case EffectNodes::Expression::LogicXor:
						part1 = '(';
						part2 = " ^^ ";
						part3 = ')';
						break;
					case EffectNodes::Expression::LogicOr:
						part1 = '(';
						part2 = " || ";
						part3 = ')';
						break;
					case EffectNodes::Expression::Mul:
						part1 = "mul(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Atan2:
						part1 = "atan2(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Dot:
						part1 = "dot(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Cross:
						part1 = "cross(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Distance:
						part1 = "distance(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Pow:
						part1 = "pow(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Modf:
						part1 = "modf(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Frexp:
						part1 = "frexp(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Ldexp:
						part1 = "ldexp(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Min:
						part1 = "min(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Max:
						part1 = "max(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Step:
						part1 = "step(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Reflect:
						part1 = "reflect(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Extract:
						part2 = '[';
						part3 = ']';
						break;
					case EffectNodes::Expression::Field:
						this->mCurrentSource += '(';
						Visit(this->mAST[node.Operands[0]]);
						this->mCurrentSource += (this->mAST[node.Operands[0]].Is<EffectNodes::LValue>() && this->mAST[node.Operands[0]].As<EffectNodes::LValue>().Type.HasQualifier(EffectNodes::Type::Uniform)) ? '_' : '.';
						this->mCurrentSource += this->mAST[node.Operands[1]].As<EffectNodes::Variable>().Name;
						this->mCurrentSource += ')';
						return;
					case EffectNodes::Expression::Tex:
						part1 = "__tex2D(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::TexLevel:
						part1 = "__tex2Dlod(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::TexGather:
						part1 = "__tex2Dgather(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::TexBias:
						part1 = "__tex2Dbias(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::TexFetch:
						part1 = "__tex2Dfetch(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::TexSize:
						part1 = "__tex2Dsize(";
						part2 = ", ";
						part3 = ")";
						break;
					case EffectNodes::Expression::Mad:
						part1 = "((";
						part2 = ") * (";
						part3 = ") + (";
						part4 = "))";
						break;
					case EffectNodes::Expression::SinCos:
						part1 = "sincos(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::Lerp:
						part1 = "lerp(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::Clamp:
						part1 = "clamp(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::SmoothStep:
						part1 = "smoothstep(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::Refract:
						part1 = "refract(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::FaceForward:
						part1 = "faceforward(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::Conditional:
						part1 = '(';
						part2 = " ? ";
						part3 = " : ";
						part4 = ')';
						break;
					case EffectNodes::Expression::TexOffset:
						part1 = "__tex2Doffset(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::TexLevelOffset:
						part1 = "__tex2Dlodoffset(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
					case EffectNodes::Expression::TexGatherOffset:
						part1 = "__tex2Dgatheroffset(";
						part2 = ", ";
						part3 = ", ";
						part4 = ")";
						break;
				}

				this->mCurrentSource += part1;
				Visit(this->mAST[node.Operands[0]]);
				this->mCurrentSource += part2;

				if (node.Operands[1] != 0)
				{
					Visit(this->mAST[node.Operands[1]]);
				}

				this->mCurrentSource += part3;

				if (node.Operands[2] != 0)
				{
					Visit(this->mAST[node.Operands[2]]);
				}

				this->mCurrentSource += part4;
			}
			void Visit(const EffectNodes::Sequence &node)
			{
				const EffectNodes::RValue *expression = &this->mAST[node.Expressions].As<EffectNodes::RValue>();

				do
				{
					Visit(*expression);

					if (expression->NextExpression != EffectTree::Null)
					{
						this->mCurrentSource += ", ";

						expression = &this->mAST[expression->NextExpression].As<EffectNodes::RValue>();
					}
					else
					{
						expression = nullptr;
					}
				}
				while (expression != nullptr);
			}
			void Visit(const EffectNodes::Assignment &node)
			{
				this->mCurrentSource += '(';
				Visit(this->mAST[node.Left]);
				this->mCurrentSource += ' ';

				switch (node.Operator)
				{
					case EffectNodes::Expression::None:
						this->mCurrentSource += '=';
						break;
					case EffectNodes::Expression::Add:
						this->mCurrentSource += "+=";
						break;
					case EffectNodes::Expression::Subtract:
						this->mCurrentSource += "-=";
						break;
					case EffectNodes::Expression::Multiply:
						this->mCurrentSource += "*=";
						break;
					case EffectNodes::Expression::Divide:
						this->mCurrentSource += "/=";
						break;
					case EffectNodes::Expression::Modulo:
						this->mCurrentSource += "%=";
						break;
					case EffectNodes::Expression::LeftShift:
						this->mCurrentSource += "<<=";
						break;
					case EffectNodes::Expression::RightShift:
						this->mCurrentSource += ">>=";
						break;
					case EffectNodes::Expression::BitAnd:
						this->mCurrentSource += "&=";
						break;
					case EffectNodes::Expression::BitXor:
						this->mCurrentSource += "^=";
						break;
					case EffectNodes::Expression::BitOr:
						this->mCurrentSource += "|=";
						break;
				}

				this->mCurrentSource += ' ';
				Visit(this->mAST[node.Right]);
				this->mCurrentSource += ')';
			}
			void Visit(const EffectNodes::Call &node)
			{
				this->mCurrentSource += node.CalleeName;
				this->mCurrentSource += '(';

				if (node.Arguments != EffectTree::Null)
				{
					const EffectNodes::RValue *argument = &this->mAST[node.Arguments].As<EffectNodes::RValue>();

					do
					{
						Visit(*argument);

						if (argument->NextExpression != EffectTree::Null)
						{
							this->mCurrentSource += ", ";

							argument = &this->mAST[argument->NextExpression].As<EffectNodes::RValue>();
						}
						else
						{
							argument = nullptr;
						}
					}
					while (argument != nullptr);
				}

				this->mCurrentSource += ')';
			}
			void Visit(const EffectNodes::Constructor &node)
			{
				this->mCurrentSource += PrintType(node.Type);
				this->mCurrentSource += '(';

				const EffectNodes::RValue *argument = &this->mAST[node.Arguments].As<EffectNodes::RValue>();

				do
				{
					Visit(*argument);

					if (argument->NextExpression != EffectTree::Null)
					{
						this->mCurrentSource += ", ";

						argument = &this->mAST[argument->NextExpression].As<EffectNodes::RValue>();
					}
					else
					{
						argument = nullptr;
					}
				}
				while (argument != nullptr);

				this->mCurrentSource += ')';
			}
			void Visit(const EffectNodes::Swizzle &node)
			{
				const EffectNodes::RValue &left = this->mAST[node.Operands[0]].As<EffectNodes::RValue>();

				Visit(left);

				this->mCurrentSource += '.';

				if (left.Type.IsMatrix())
				{
					const char swizzle[16][5] =
					{
						"_m00", "_m01", "_m02", "_m03",
						"_m10", "_m11", "_m12", "_m13",
						"_m20", "_m21", "_m22", "_m23",
						"_m30", "_m31", "_m32", "_m33"
					};

					for (int i = 0; i < 4 && node.Mask[i] >= 0; ++i)
					{
						this->mCurrentSource += swizzle[node.Mask[i]];
					}
				}
				else
				{
					const char swizzle[4] =
					{
						'x', 'y', 'z', 'w'
					};

					for (int i = 0; i < 4 && node.Mask[i] >= 0; ++i)
					{
						this->mCurrentSource += swizzle[node.Mask[i]];
					}
				}
			}
			void Visit(const EffectNodes::InitializerList &node)
			{
				this->mCurrentSource += "{ ";

				const EffectNodes::RValue *expression = &this->mAST[node.Expressions].As<EffectNodes::RValue>();

				do
				{
					Visit(*expression);

					if (expression->NextExpression != EffectTree::Null)
					{
						this->mCurrentSource += ", ";

						expression = &this->mAST[expression->NextExpression].As<EffectNodes::RValue>();
					}
					else
					{
						expression = nullptr;
					}
				}
				while (expression != nullptr);

				this->mCurrentSource += " }";
			}
			void Visit(const EffectNodes::If &node)
			{
				if (node.Attributes != nullptr)
				{
					this->mCurrentSource += '[';
					this->mCurrentSource += node.Attributes;
					this->mCurrentSource += ']';
				}

				this->mCurrentSource += "if (";
				Visit(this->mAST[node.Condition]);
				this->mCurrentSource += ")\n";

				if (node.StatementOnTrue != EffectTree::Null)
				{
					Visit(this->mAST[node.StatementOnTrue]);
				}
				else
				{
					this->mCurrentSource += "\t;";
				}
				if (node.StatementOnFalse != EffectTree::Null)
				{
					this->mCurrentSource += "else\n";
					Visit(this->mAST[node.StatementOnFalse]);
				}
			}
			void Visit(const EffectNodes::Switch &node)
			{
				if (node.Attributes != nullptr)
				{
					this->mCurrentSource += '[';
					this->mCurrentSource += node.Attributes;
					this->mCurrentSource += ']';
				}

				this->mCurrentSource += "switch (";
				Visit(this->mAST[node.Test]);
				this->mCurrentSource += ")\n{\n";

				const EffectNodes::Case *cases = &this->mAST[node.Cases].As<EffectNodes::Case>();

				do
				{
					Visit(*cases);

					if (cases->NextCase != EffectTree::Null)
					{
						cases = &this->mAST[cases->NextCase].As<EffectNodes::Case>();
					}
					else
					{
						cases = nullptr;
					}
				}
				while (cases != nullptr);

				this->mCurrentSource += "}\n";
			}
			void Visit(const EffectNodes::Case &node)
			{
				const EffectNodes::RValue *label = &this->mAST[node.Labels].As<EffectNodes::RValue>();

				do
				{
					if (label->Is<EffectNodes::Expression>())
					{
						this->mCurrentSource += "default";
					}
					else
					{
						this->mCurrentSource += "case ";
						Visit(label->As<EffectNodes::Literal>());
					}

					this->mCurrentSource += ":\n";

					if (label->NextExpression != EffectTree::Null)
					{
						label = &this->mAST[label->NextExpression].As<EffectNodes::RValue>();
					}
					else
					{
						label = nullptr;
					}
				}
				while (label != nullptr);

				Visit(this->mAST[node.Statements].As<EffectNodes::StatementBlock>());
			}
			void Visit(const EffectNodes::For &node)
			{
				if (node.Attributes != nullptr)
				{
					this->mCurrentSource += '[';
					this->mCurrentSource += node.Attributes;
					this->mCurrentSource += ']';
				}

				this->mCurrentSource += "for (";

				if (node.Initialization != EffectTree::Null)
				{
					Visit(this->mAST[node.Initialization]);
				}
				else
				{
					this->mCurrentSource += "; ";
				}
										
				if (node.Condition != EffectTree::Null)
				{
					Visit(this->mAST[node.Condition]);
				}

				this->mCurrentSource += "; ";

				if (node.Iteration != EffectTree::Null)
				{
					Visit(this->mAST[node.Iteration]);
				}

				this->mCurrentSource += ")\n";

				if (node.Statements != EffectTree::Null)
				{
					Visit(this->mAST[node.Statements]);
				}
				else
				{
					this->mCurrentSource += "\t;";
				}
			}
			void Visit(const EffectNodes::While &node)
			{
				if (node.Attributes != nullptr)
				{
					this->mCurrentSource += '[';
					this->mCurrentSource += node.Attributes;
					this->mCurrentSource += ']';
				}

				if (node.DoWhile)
				{
					this->mCurrentSource += "do\n{\n";

					if (node.Statements != EffectTree::Null)
					{
						Visit(this->mAST[node.Statements]);
					}

					this->mCurrentSource += "}\n";
					this->mCurrentSource += "while (";
					Visit(this->mAST[node.Condition]);
					this->mCurrentSource += ");\n";
				}
				else
				{
					this->mCurrentSource += "while (";
					Visit(this->mAST[node.Condition]);
					this->mCurrentSource += ")\n";

					if (node.Statements != EffectTree::Null)
					{
						Visit(this->mAST[node.Statements]);
					}
					else
					{
						this->mCurrentSource += "\t;";
					}
				}
			}
			void Visit(const EffectNodes::Return &node)
			{
				if (node.Discard)
				{
					this->mCurrentSource += "discard";
				}
				else
				{
					this->mCurrentSource += "return";

					if (node.Value != EffectTree::Null)
					{
						this->mCurrentSource += ' ';
						Visit(this->mAST[node.Value]);
					}
				}

				this->mCurrentSource += ";\n";
			}
			void Visit(const EffectNodes::Jump &node)
			{
				switch (node.Mode)
				{
					case EffectNodes::Jump::Break:
						this->mCurrentSource += "break";
						break;
					case EffectNodes::Jump::Continue:
						this->mCurrentSource += "continue";
						break;
				}

				this->mCurrentSource += ";\n";
			}
			void Visit(const EffectNodes::ExpressionStatement &node)
			{
				if (node.Expression != EffectTree::Null)
				{
					Visit(this->mAST[node.Expression]);
				}

				this->mCurrentSource += ";\n";
			}
			void Visit(const EffectNodes::DeclarationStatement &node)
			{
				Visit(this->mAST[node.Declaration]);
			}
			void Visit(const EffectNodes::StatementBlock &node)
			{
				this->mCurrentSource += "{\n";

				if (node.Statements != EffectTree::Null)
				{
					const EffectNodes::Statement *statement = &this->mAST[node.Statements].As<EffectNodes::Statement>();

					do
					{
						Visit(*statement);

						if (statement->NextStatement != EffectTree::Null)
						{
							statement = &this->mAST[statement->NextStatement].As<EffectNodes::Statement>();
						}
						else
						{
							statement = nullptr;
						}
					}
					while (statement != nullptr);
				}

				this->mCurrentSource += "}\n";
			}
			void Visit(const EffectNodes::Annotation &node, D3D11Texture &texture)
			{
				Effect::Annotation data;
				const auto &value = this->mAST[node.Value].As<EffectNodes::Literal>();

				switch (value.Type.Class)
				{
					case EffectNodes::Type::Bool:
						data = value.Value.Bool[0] != 0;
						break;
					case EffectNodes::Type::Int:
						data = value.Value.Int[0];
						break;
					case EffectNodes::Type::Uint:
						data = value.Value.Uint[0];
						break;
					case EffectNodes::Type::Float:
						data = value.Value.Float[0];
						break;
					case EffectNodes::Type::String:
						data = value.Value.String;
						break;
				}

				texture.AddAnnotation(node.Name, data);

				if (node.NextAnnotation != EffectTree::Null)
				{
					Visit(this->mAST[node.NextAnnotation].As<EffectNodes::Annotation>(), texture);
				}
			}
			void Visit(const EffectNodes::Annotation &node, D3D11Constant &constant)
			{
				Effect::Annotation data;
				const auto &value = this->mAST[node.Value].As<EffectNodes::Literal>();

				switch (value.Type.Class)
				{
					case EffectNodes::Type::Bool:
						data = value.Value.Bool[0] != 0;
						break;
					case EffectNodes::Type::Int:
						data = value.Value.Int[0];
						break;
					case EffectNodes::Type::Uint:
						data = value.Value.Uint[0];
						break;
					case EffectNodes::Type::Float:
						data = value.Value.Float[0];
						break;
					case EffectNodes::Type::String:
						data = value.Value.String;
						break;
				}

				constant.AddAnnotation(node.Name, data);

				if (node.NextAnnotation != EffectTree::Null)
				{
					Visit(this->mAST[node.NextAnnotation].As<EffectNodes::Annotation>(), constant);
				}
			}
			void Visit(const EffectNodes::Annotation &node, D3D11Technique &technique)
			{
				Effect::Annotation data;
				const auto &value = this->mAST[node.Value].As<EffectNodes::Literal>();

				switch (value.Type.Class)
				{
					case EffectNodes::Type::Bool:
						data = value.Value.Bool[0] != 0;
						break;
					case EffectNodes::Type::Int:
						data = value.Value.Int[0];
						break;
					case EffectNodes::Type::Uint:
						data = value.Value.Uint[0];
						break;
					case EffectNodes::Type::Float:
						data = value.Value.Float[0];
						break;
					case EffectNodes::Type::String:
						data = value.Value.String;
						break;
				}

				technique.AddAnnotation(node.Name, data);

				if (node.NextAnnotation != EffectTree::Null)
				{
					Visit(this->mAST[node.NextAnnotation].As<EffectNodes::Annotation>(), technique);
				}
			}
			void Visit(const EffectNodes::Struct &node)
			{
				this->mCurrentSource += "struct ";

				if (node.Name != nullptr)
				{
					this->mCurrentSource += node.Name;
				}

				this->mCurrentSource += "\n{\n";

				if (node.Fields != EffectTree::Null)
				{
					Visit(this->mAST[node.Fields].As<EffectNodes::Variable>());
				}
				else
				{
					this->mCurrentSource += "float _dummy;\n";
				}

				this->mCurrentSource += "};\n";
			}
			void Visit(const EffectNodes::Variable &node)
			{
				if (!(this->mCurrentInParameterBlock || this->mCurrentInFunctionBlock))
				{
					if (node.Type.IsStruct() && node.Type.HasQualifier(EffectNodes::Type::Qualifier::Uniform))
					{
						VisitUniformBuffer(node);
						return;
					}
					else if (node.Type.IsTexture())
					{
						VisitTexture(node);
						return;
					}
					else if (node.Type.IsSampler())
					{
						VisitSampler(node);
						return;
					}
					else if (node.Type.HasQualifier(EffectNodes::Type::Qualifier::Uniform))
					{
						VisitUniform(node);
						return;
					}
				}

				if (!this->mCurrentInDeclaratorList)
				{
					this->mCurrentSource += PrintTypeWithQualifiers(node.Type);
				}

				if (node.Name != nullptr)
				{
					this->mCurrentSource += ' ';

					if (!this->mCurrentBlockName.empty())
					{
						this->mCurrentSource += this->mCurrentBlockName + '_';
					}
				
					this->mCurrentSource += node.Name;
				}

				if (node.Type.IsArray())
				{
					this->mCurrentSource += '[';
					this->mCurrentSource += (node.Type.ArrayLength >= 1) ? std::to_string(node.Type.ArrayLength) : "";
					this->mCurrentSource += ']';
				}

				if (node.Semantic != nullptr)
				{
					this->mCurrentSource += " : " + ConvertSemantic(node.Semantic);
				}

				if (node.Initializer != EffectTree::Null)
				{
					this->mCurrentSource += " = ";

					Visit(this->mAST[node.Initializer]);
				}

				if (node.NextDeclarator != EffectTree::Null)
				{
					const auto &next = this->mAST[node.NextDeclarator].As<EffectNodes::Variable>();

					if (next.Type.Class == node.Type.Class && next.Type.Rows == node.Type.Rows && next.Type.Cols == node.Type.Rows && next.Type.Definition == node.Type.Definition)
					{
						this->mCurrentSource += ", ";

						this->mCurrentInDeclaratorList = true;

						Visit(next);

						this->mCurrentInDeclaratorList = false;
					}
					else
					{
						this->mCurrentSource += ";\n";

						Visit(next);
					}
				}
				else if (!this->mCurrentInParameterBlock)
				{
					this->mCurrentSource += ";\n";
				}
			}
			void VisitTexture(const EffectNodes::Variable &node)
			{
				D3D11_TEXTURE2D_DESC texdesc;
				ZeroMemory(&texdesc, sizeof(D3D11_TEXTURE2D_DESC));
				texdesc.Width = (node.Properties[EffectNodes::Variable::Width] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::Width]].As<EffectNodes::Literal>().Value.Uint[0] : 1;
				texdesc.Height = (node.Properties[EffectNodes::Variable::Height] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::Height]].As<EffectNodes::Literal>().Value.Uint[0] : 1;
				texdesc.MipLevels = (node.Properties[EffectNodes::Variable::MipLevels] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::MipLevels]].As<EffectNodes::Literal>().Value.Uint[0] : 1;
				texdesc.ArraySize = 1;
				texdesc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
				texdesc.SampleDesc.Count = 1;
				texdesc.SampleDesc.Quality = 0;
				texdesc.Usage = D3D11_USAGE_DEFAULT;
				texdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				texdesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
				
				Effect::Texture::Format format = Effect::Texture::Format::RGBA8;

				if (node.Properties[EffectNodes::Variable::Format] != 0)
				{
					texdesc.Format = LiteralToFormat(this->mAST[node.Properties[EffectNodes::Variable::Format]].As<EffectNodes::Literal>().Value.Uint[0], format);
				}

				D3D11Texture::Description objdesc;
				objdesc.Width = texdesc.Width;
				objdesc.Height = texdesc.Height;
				objdesc.Levels = texdesc.MipLevels;
				objdesc.Format = format;

				D3D11Texture *obj = new D3D11Texture(this->mEffect, objdesc);
				obj->mRegister = this->mEffect->mShaderResources.size();
				obj->mTexture = nullptr;
				obj->mShaderResourceView[0] = nullptr;
				obj->mShaderResourceView[1] = nullptr;

				if (node.Semantic != nullptr)
				{
					if (boost::equals(node.Semantic, "COLOR") || boost::equals(node.Semantic, "SV_TARGET"))
					{
						obj->mSource = D3D11Texture::Source::BackBuffer;
						obj->ChangeSource(this->mEffect->mRuntime->mBackBufferTextureSRV[0], this->mEffect->mRuntime->mBackBufferTextureSRV[1]);
					}
					else if (boost::equals(node.Semantic, "DEPTH") || boost::equals(node.Semantic, "SV_DEPTH"))
					{
						obj->mSource = D3D11Texture::Source::DepthStencil;
						obj->ChangeSource(this->mEffect->mRuntime->mDepthStencilTextureSRV, nullptr);
					}
				}

				if (obj->mSource != D3D11Texture::Source::Memory)
				{
					if (texdesc.Width != 1 || texdesc.Height != 1 || texdesc.MipLevels != 1 || texdesc.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)
					{
						this->mErrors += PrintLocation(node.Location) + "warning: texture property on backbuffer textures are ignored.\n";
					}
				}
				else
				{
					HRESULT hr = this->mEffect->mRuntime->mDevice->CreateTexture2D(&texdesc, nullptr, &obj->mTexture);

					if (FAILED(hr))
					{
						this->mErrors += PrintLocation(node.Location) + "error: 'CreateTexture2D' failed!\n";
						this->mFatal = true;
						return;
					}

					D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc;
					ZeroMemory(&srvdesc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
					srvdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srvdesc.Texture2D.MipLevels = texdesc.MipLevels;
					srvdesc.Format = MakeNonSRBFormat(texdesc.Format);

					hr = this->mEffect->mRuntime->mDevice->CreateShaderResourceView(obj->mTexture, &srvdesc, &obj->mShaderResourceView[0]);

					if (FAILED(hr))
					{
						this->mErrors += PrintLocation(node.Location) + "error: 'CreateShaderResourceView' failed!\n";
						this->mFatal = true;
						return;
					}

					srvdesc.Format = MakeSRGBFormat(texdesc.Format);

					if (srvdesc.Format != texdesc.Format)
					{
						hr = this->mEffect->mRuntime->mDevice->CreateShaderResourceView(obj->mTexture, &srvdesc, &obj->mShaderResourceView[1]);

						if (FAILED(hr))
						{
							this->mErrors += PrintLocation(node.Location) + "error: 'CreateShaderResourceView' failed!\n";
							this->mFatal = true;
							return;
						}
					}
				}

				if (node.Annotations != EffectTree::Null)
				{
					Visit(this->mAST[node.Annotations].As<EffectNodes::Annotation>(), *obj);
				}

				this->mCurrentSource += "Texture2D ";
				this->mCurrentSource += node.Name;
				this->mCurrentSource += " : register(t" + std::to_string(this->mEffect->mShaderResources.size()) + "), __";
				this->mCurrentSource += node.Name;
				this->mCurrentSource += "SRGB : register(t" + std::to_string(this->mEffect->mShaderResources.size() + 1) + ");\n";

				this->mEffect->mShaderResources.push_back(obj->mShaderResourceView[0]);
				this->mEffect->mShaderResources.push_back(obj->mShaderResourceView[1]);

				this->mEffect->AddTexture(node.Name, obj);
			}
			void VisitSampler(const EffectNodes::Variable &node)
			{
				if (node.Properties[EffectNodes::Variable::Texture] == 0)
				{
					this->mErrors += PrintLocation(node.Location) + "error: sampler '" + std::string(node.Name) + "' is missing required 'Texture' required.\n";
					this->mFatal = true;
					return;
				}

				D3D11_SAMPLER_DESC desc;
				desc.AddressU = (node.Properties[EffectNodes::Variable::AddressU] != 0) ? LiteralToTextureAddress(this->mAST[node.Properties[EffectNodes::Variable::AddressU]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.AddressV = (node.Properties[EffectNodes::Variable::AddressV] != 0) ? LiteralToTextureAddress(this->mAST[node.Properties[EffectNodes::Variable::AddressV]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.AddressW = (node.Properties[EffectNodes::Variable::AddressW] != 0) ? LiteralToTextureAddress(this->mAST[node.Properties[EffectNodes::Variable::AddressW]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_TEXTURE_ADDRESS_CLAMP;
				desc.MipLODBias = (node.Properties[EffectNodes::Variable::MipLODBias] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::MipLODBias]].As<EffectNodes::Literal>().Value.Float[0] : 0.0f;
				desc.MinLOD = (node.Properties[EffectNodes::Variable::MinLOD] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::MinLOD]].As<EffectNodes::Literal>().Value.Float[0] : -FLT_MAX;
				desc.MaxLOD = (node.Properties[EffectNodes::Variable::MaxLOD] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::MaxLOD]].As<EffectNodes::Literal>().Value.Float[0] : FLT_MAX;
				desc.MaxAnisotropy = (node.Properties[EffectNodes::Variable::MaxAnisotropy] != 0) ? this->mAST[node.Properties[EffectNodes::Variable::MaxAnisotropy]].As<EffectNodes::Literal>().Value.Uint[0] : 1;
				desc.ComparisonFunc = D3D11_COMPARISON_NEVER;

				UINT minfilter = EffectNodes::Literal::LINEAR, magfilter = EffectNodes::Literal::LINEAR, mipfilter = EffectNodes::Literal::LINEAR;

				if (node.Properties[EffectNodes::Variable::MinFilter] != 0)
				{
					minfilter = this->mAST[node.Properties[EffectNodes::Variable::MinFilter]].As<EffectNodes::Literal>().Value.Uint[0];
				}
				if (node.Properties[EffectNodes::Variable::MagFilter] != 0)
				{
					magfilter = this->mAST[node.Properties[EffectNodes::Variable::MagFilter]].As<EffectNodes::Literal>().Value.Uint[0];
				}
				if (node.Properties[EffectNodes::Variable::MipFilter] != 0)
				{
					mipfilter = this->mAST[node.Properties[EffectNodes::Variable::MipFilter]].As<EffectNodes::Literal>().Value.Uint[0];
				}

				if (minfilter == EffectNodes::Literal::ANISOTROPIC || magfilter == EffectNodes::Literal::ANISOTROPIC || mipfilter == EffectNodes::Literal::ANISOTROPIC)
				{
					desc.Filter = D3D11_FILTER_ANISOTROPIC;
				}
				else if (minfilter == EffectNodes::Literal::POINT && magfilter == EffectNodes::Literal::POINT && mipfilter == EffectNodes::Literal::LINEAR)
				{
					desc.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
				}
				else if (minfilter == EffectNodes::Literal::POINT && magfilter == EffectNodes::Literal::LINEAR && mipfilter == EffectNodes::Literal::POINT)
				{
					desc.Filter = D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
				}
				else if (minfilter == EffectNodes::Literal::POINT && magfilter == EffectNodes::Literal::LINEAR && mipfilter == EffectNodes::Literal::LINEAR)
				{
					desc.Filter = D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
				}
				else if (minfilter == EffectNodes::Literal::LINEAR && magfilter == EffectNodes::Literal::POINT && mipfilter == EffectNodes::Literal::POINT)
				{
					desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
				}
				else if (minfilter == EffectNodes::Literal::LINEAR && magfilter == EffectNodes::Literal::POINT && mipfilter == EffectNodes::Literal::LINEAR)
				{
					desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
				}
				else if (minfilter == EffectNodes::Literal::LINEAR && magfilter == EffectNodes::Literal::LINEAR && mipfilter == EffectNodes::Literal::POINT)
				{
					desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
				}
				else if (minfilter == EffectNodes::Literal::LINEAR && magfilter == EffectNodes::Literal::LINEAR && mipfilter == EffectNodes::Literal::LINEAR)
				{
					desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				}
				else
				{
					desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
				}

				bool srgb = false;

				if (node.Properties[EffectNodes::Variable::SRGBTexture] != 0)
				{
					srgb = this->mAST[node.Properties[EffectNodes::Variable::SRGBTexture]].As<EffectNodes::Literal>().Value.Bool[0] != 0;
				}

				const char *textureName = this->mAST[node.Properties[EffectNodes::Variable::Texture]].As<EffectNodes::Variable>().Name;
				D3D11Texture *texture = static_cast<D3D11Texture *>(this->mEffect->GetTexture(textureName));

				if (texture == nullptr)
				{
					this->mErrors += PrintLocation(node.Location) + "error: texture '" + std::string(textureName) + "' for sampler '" + std::string(node.Name) + "' is missing.\n";
					this->mFatal = true;
					return;
				}

				const std::size_t descHash = D3D11_SAMPLER_DESC_HASH(desc);
				auto it = this->mSamplerDescs.find(descHash);

				if (it == this->mSamplerDescs.end())
				{
					ID3D11SamplerState *sampler = nullptr;

					HRESULT hr = this->mEffect->mRuntime->mDevice->CreateSamplerState(&desc, &sampler);

					if (FAILED(hr))
					{
						this->mErrors += PrintLocation(node.Location) + "error: 'CreateSamplerState' failed!\n";
						this->mFatal = true;
						return;
					}

					this->mEffect->mSamplerStates.push_back(sampler);
					it = this->mSamplerDescs.emplace(descHash, this->mEffect->mSamplerStates.size() - 1).first;

					this->mCurrentSource += "SamplerState __SamplerState" + std::to_string(it->second) + " : register(s" + std::to_string(it->second) + ");\n";
				}

				this->mCurrentSource += "static const __sampler2D ";
				this->mCurrentSource += node.Name;
				this->mCurrentSource += " = { ";

				if (srgb && texture->mShaderResourceView[1] != nullptr)
				{
					this->mCurrentSource += "__";
					this->mCurrentSource += textureName;
					this->mCurrentSource += "SRGB";
				}
				else
				{
					this->mCurrentSource += textureName;
				}

				this->mCurrentSource += ", __SamplerState" + std::to_string(it->second) + " };\n";
			}
			void VisitUniform(const EffectNodes::Variable &node)
			{
				this->mCurrentGlobalConstants += PrintTypeWithQualifiers(node.Type);
				this->mCurrentGlobalConstants += ' ';
				this->mCurrentGlobalConstants += node.Name;

				if (node.Type.IsArray())
				{
					this->mCurrentGlobalConstants += '[';
					this->mCurrentGlobalConstants += (node.Type.ArrayLength >= 1) ? std::to_string(node.Type.ArrayLength) : "";
					this->mCurrentGlobalConstants += ']';
				}

				this->mCurrentGlobalConstants += ";\n";

				D3D11Constant::Description objdesc;
				objdesc.Rows = node.Type.Rows;
				objdesc.Columns = node.Type.Cols;
				objdesc.Elements = node.Type.ArrayLength;
				objdesc.Fields = 0;
				objdesc.Size = node.Type.Rows * node.Type.Cols;

				switch (node.Type.Class)
				{
					case EffectNodes::Type::Bool:
						objdesc.Size *= sizeof(int);
						objdesc.Type = Effect::Constant::Type::Bool;
						break;
					case EffectNodes::Type::Int:
						objdesc.Size *= sizeof(int);
						objdesc.Type = Effect::Constant::Type::Int;
						break;
					case EffectNodes::Type::Uint:
						objdesc.Size *= sizeof(unsigned int);
						objdesc.Type = Effect::Constant::Type::Uint;
						break;
					case EffectNodes::Type::Float:
						objdesc.Size *= sizeof(float);
						objdesc.Type = Effect::Constant::Type::Float;
						break;
				}

				const UINT alignment = 16 - (this->mCurrentGlobalSize % 16);
				this->mCurrentGlobalSize += static_cast<UINT>((objdesc.Size > alignment && (alignment != 16 || objdesc.Size <= 16)) ? objdesc.Size + alignment : objdesc.Size);

				D3D11Constant *obj = new D3D11Constant(this->mEffect, objdesc);
				obj->mBufferIndex = 0;
				obj->mBufferOffset = this->mCurrentGlobalSize - objdesc.Size;

				if (this->mCurrentGlobalSize >= this->mCurrentGlobalStorageSize)
				{
					this->mEffect->mConstantStorages[0] = static_cast<unsigned char *>(::realloc(this->mEffect->mConstantStorages[0], this->mCurrentGlobalStorageSize += 128));
				}

				if (node.Annotations != EffectTree::Null)
				{
					Visit(this->mAST[node.Annotations].As<EffectNodes::Annotation>(), *obj);
				}

				if (node.Initializer != EffectTree::Null && this->mAST[node.Initializer].Is<EffectNodes::Literal>())
				{
					CopyMemory(this->mEffect->mConstantStorages[0] + obj->mBufferOffset, &this->mAST[node.Initializer].As<EffectNodes::Literal>().Value, objdesc.Size);
				}
				else
				{
					ZeroMemory(this->mEffect->mConstantStorages[0] + obj->mBufferOffset, objdesc.Size);
				}

				this->mEffect->AddConstant(node.Name, obj);
			}
			void VisitUniformBuffer(const EffectNodes::Variable &node)
			{
				const auto &structure = this->mAST[node.Type.Definition].As<EffectNodes::Struct>();

				if (!structure.Fields != EffectTree::Null)
				{
					return;
				}

				this->mCurrentSource += "cbuffer ";
				this->mCurrentSource += node.Name;
				this->mCurrentSource += " : register(b" + std::to_string(this->mEffect->mConstantBuffers.size()) + ")";
				this->mCurrentSource += "\n{\n";

				this->mCurrentBlockName = node.Name;

				ID3D11Buffer *buffer = nullptr;
				unsigned char *storage = nullptr;
				UINT totalsize = 0, currentsize = 0;

				unsigned int fieldCount = 0;
				const EffectNodes::Variable *field = &this->mAST[structure.Fields].As<EffectNodes::Variable>();

				do
				{
					fieldCount++;

					Visit(*field);

					D3D11Constant::Description objdesc;
					objdesc.Rows = field->Type.Rows;
					objdesc.Columns = field->Type.Cols;
					objdesc.Elements = field->Type.ArrayLength;
					objdesc.Fields = 0;
					objdesc.Size = field->Type.Rows * field->Type.Cols;

					switch (field->Type.Class)
					{
						case EffectNodes::Type::Bool:
							objdesc.Size *= sizeof(int);
							objdesc.Type = Effect::Constant::Type::Bool;
							break;
						case EffectNodes::Type::Int:
							objdesc.Size *= sizeof(int);
							objdesc.Type = Effect::Constant::Type::Int;
							break;
						case EffectNodes::Type::Uint:
							objdesc.Size *= sizeof(unsigned int);
							objdesc.Type = Effect::Constant::Type::Uint;
							break;
						case EffectNodes::Type::Float:
							objdesc.Size *= sizeof(float);
							objdesc.Type = Effect::Constant::Type::Float;
							break;
					}

					const UINT alignment = 16 - (totalsize % 16);
					totalsize += static_cast<UINT>((objdesc.Size > alignment && (alignment != 16 || objdesc.Size <= 16)) ? objdesc.Size + alignment : objdesc.Size);

					D3D11Constant *obj = new D3D11Constant(this->mEffect, objdesc);
					obj->mBufferIndex = this->mEffect->mConstantBuffers.size();
					obj->mBufferOffset = totalsize - objdesc.Size;

					if (totalsize >= currentsize)
					{
						storage = static_cast<unsigned char *>(::realloc(storage, currentsize += 128));
					}

					if (field->Initializer != EffectTree::Null && this->mAST[field->Initializer].Is<EffectNodes::Literal>())
					{
						CopyMemory(storage + obj->mBufferOffset, &this->mAST[field->Initializer].As<EffectNodes::Literal>().Value, objdesc.Size);
					}
					else
					{
						ZeroMemory(storage + obj->mBufferOffset, objdesc.Size);
					}

					this->mEffect->AddConstant(std::string(node.Name) + '.' + std::string(field->Name), obj);

					if (field->NextDeclarator != EffectTree::Null)
					{
						field = &this->mAST[field->NextDeclarator].As<EffectNodes::Variable>();
					}
					else
					{
						field = nullptr;
					}
				}
				while (field != nullptr);

				this->mCurrentBlockName.clear();

				this->mCurrentSource += "};\n";

				D3D11Constant::Description objdesc;
				objdesc.Rows = 0;
				objdesc.Columns = 0;
				objdesc.Elements = 0;
				objdesc.Fields = fieldCount;
				objdesc.Size = totalsize;
				objdesc.Type = Effect::Constant::Type::Struct;

				D3D11Constant *obj = new D3D11Constant(this->mEffect, objdesc);
				obj->mBufferIndex = this->mEffect->mConstantBuffers.size();
				obj->mBufferOffset = 0;

				if (node.Annotations != EffectTree::Null)
				{
					Visit(this->mAST[node.Annotations].As<EffectNodes::Annotation>(), *obj);
				}

				this->mEffect->AddConstant(node.Name, obj);

				D3D11_BUFFER_DESC desc;
				desc.ByteWidth = RoundToMultipleOf16(totalsize);
				desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				desc.Usage = D3D11_USAGE_DYNAMIC;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				desc.MiscFlags = 0;
				desc.StructureByteStride = 0;

				D3D11_SUBRESOURCE_DATA initial;
				initial.pSysMem = storage;
				initial.SysMemPitch = initial.SysMemSlicePitch = totalsize;

				HRESULT hr = this->mEffect->mRuntime->mDevice->CreateBuffer(&desc, &initial, &buffer);

				if (SUCCEEDED(hr))
				{
					this->mEffect->mConstantBuffers.push_back(buffer);
					this->mEffect->mConstantStorages.push_back(storage);
				}
			}
			void Visit(const EffectNodes::Function &node)
			{
				this->mCurrentSource += PrintType(node.ReturnType);
				this->mCurrentSource += ' ';
				this->mCurrentSource += node.Name;
				this->mCurrentSource += '(';

				if (node.Parameters != EffectTree::Null)
				{
					this->mCurrentInParameterBlock = true;

					const EffectNodes::Variable *parameter = &this->mAST[node.Parameters].As<EffectNodes::Variable>();

					do
					{
						Visit(*parameter);

						if (parameter->NextDeclaration != EffectTree::Null)
						{
							this->mCurrentSource += ", ";

							parameter = &this->mAST[parameter->NextDeclaration].As<EffectNodes::Variable>();
						}
						else
						{
							parameter = nullptr;
						}
					}
					while (parameter != nullptr);

					this->mCurrentInParameterBlock = false;
				}

				this->mCurrentSource += ')';
								
				if (node.ReturnSemantic != nullptr)
				{
					this->mCurrentSource += " : " + ConvertSemantic(node.ReturnSemantic);
				}

				if (node.Definition != EffectTree::Null)
				{
					this->mCurrentSource += '\n';

					this->mCurrentInFunctionBlock = true;

					Visit(this->mAST[node.Definition].As<EffectNodes::StatementBlock>());

					this->mCurrentInFunctionBlock = false;
				}
				else
				{
					this->mCurrentSource += ";\n";
				}
			}
			void Visit(const EffectNodes::Technique &node)
			{
				std::vector<D3D11Technique::Pass> passes;
				const EffectNodes::Pass *pass = &this->mAST[node.Passes].As<EffectNodes::Pass>();

				do
				{
					Visit(*pass, passes);

					if (pass->NextPass != EffectTree::Null)
					{
						pass = &this->mAST[pass->NextPass].As<EffectNodes::Pass>();
					}
					else
					{
						pass = nullptr;
					}
				}
				while (pass != nullptr);

				D3D11Technique::Description objdesc;
				objdesc.Passes = static_cast<unsigned int>(passes.size());

				D3D11Technique *obj = new D3D11Technique(this->mEffect, objdesc);
				obj->mPasses = std::move(passes);

				if (node.Annotations != EffectTree::Null)
				{
					Visit(this->mAST[node.Annotations].As<EffectNodes::Annotation>(), *obj);
				}

				this->mEffect->AddTechnique(node.Name, obj);
			}
			void Visit(const EffectNodes::Pass &node, std::vector<D3D11Technique::Pass> &passes)
			{
				D3D11Technique::Pass pass;
				pass.VS = nullptr;
				pass.PS = nullptr;
				pass.BS = nullptr;
				pass.DSS = nullptr;
				pass.StencilRef = 0;
				pass.Viewport.TopLeftX = pass.Viewport.TopLeftY = pass.Viewport.Width = pass.Viewport.Height = 0.0f;;
				pass.Viewport.MinDepth = 0.0f;
				pass.Viewport.MaxDepth = 1.0f;
				ZeroMemory(pass.RT, sizeof(pass.RT));
				ZeroMemory(pass.RTSRV, sizeof(pass.RTSRV));
				pass.SRV = this->mEffect->mShaderResources;

				if (node.States[EffectNodes::Pass::VertexShader] != 0)
				{
					VisitShader(this->mAST[node.States[EffectNodes::Pass::VertexShader]].As<EffectNodes::Function>(), EffectNodes::Pass::VertexShader, pass);
				}
				if (node.States[EffectNodes::Pass::PixelShader] != 0)
				{
					VisitShader(this->mAST[node.States[EffectNodes::Pass::PixelShader]].As<EffectNodes::Function>(), EffectNodes::Pass::PixelShader, pass);
				}

				int srgb = 0;

				if (node.States[EffectNodes::Pass::SRGBWriteEnable] != 0)
				{
					srgb = this->mAST[node.States[EffectNodes::Pass::SRGBWriteEnable]].As<EffectNodes::Literal>().Value.Bool[0];
				}

				pass.RT[0] = this->mEffect->mRuntime->mBackBufferTargets[srgb];
				pass.RTSRV[0] = this->mEffect->mRuntime->mBackBufferTextureSRV[srgb];

				for (unsigned int i = 0; i < 8; ++i)
				{
					if (node.States[EffectNodes::Pass::RenderTarget0 + i] != 0)
					{
						const char *textureName = this->mAST[node.States[EffectNodes::Pass::RenderTarget0 + i]].As<EffectNodes::Variable>().Name;
						D3D11Texture *texture = static_cast<D3D11Texture *>(this->mEffect->GetTexture(textureName));

						if (texture == nullptr)
						{
							this->mFatal = true;
							return;
						}

						D3D11_TEXTURE2D_DESC desc;
						texture->mTexture->GetDesc(&desc);

						if (pass.Viewport.Width != 0 && pass.Viewport.Height != 0 && (desc.Width != static_cast<unsigned int>(pass.Viewport.Width) || desc.Height != static_cast<unsigned int>(pass.Viewport.Height)))
						{
							this->mErrors += PrintLocation(node.Location) + "error: cannot use multiple rendertargets with different sized textures.\n";
							this->mFatal = true;
							return;
						}
						else
						{
							pass.Viewport.Width = static_cast<FLOAT>(desc.Width);
							pass.Viewport.Height = static_cast<FLOAT>(desc.Height);
						}

						D3D11_RENDER_TARGET_VIEW_DESC rtvdesc;
						ZeroMemory(&rtvdesc, sizeof(D3D11_RENDER_TARGET_VIEW_DESC));
						rtvdesc.Format = srgb ? MakeSRGBFormat(desc.Format) : MakeNonSRBFormat(desc.Format);
						rtvdesc.ViewDimension = desc.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;

						if (texture->mRenderTargetView[srgb] == nullptr)
						{
							HRESULT hr = this->mEffect->mRuntime->mDevice->CreateRenderTargetView(texture->mTexture, &rtvdesc, &texture->mRenderTargetView[srgb]);

							if (FAILED(hr))
							{
								this->mErrors += PrintLocation(node.Location) + "warning: 'CreateRenderTargetView' failed!\n";
							}
						}

						pass.RT[i] = texture->mRenderTargetView[srgb];
						pass.RTSRV[i] = texture->mShaderResourceView[srgb];
					}
				}

				if (pass.Viewport.Width == 0 && pass.Viewport.Height == 0)
				{
					pass.Viewport.Width = static_cast<FLOAT>(this->mEffect->mRuntime->mSwapChainDesc.BufferDesc.Width);
					pass.Viewport.Height = static_cast<FLOAT>(this->mEffect->mRuntime->mSwapChainDesc.BufferDesc.Height);
				}

				D3D11_DEPTH_STENCIL_DESC ddesc;
				ddesc.DepthEnable = node.States[EffectNodes::Pass::DepthEnable] != 0 && this->mAST[node.States[EffectNodes::Pass::DepthEnable]].As<EffectNodes::Literal>().Value.Bool[0];
				ddesc.DepthWriteMask = (node.States[EffectNodes::Pass::DepthWriteMask] == 0 || this->mAST[node.States[EffectNodes::Pass::DepthWriteMask]].As<EffectNodes::Literal>().Value.Bool[0]) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
				ddesc.DepthFunc = (node.States[EffectNodes::Pass::DepthFunc] != 0) ? LiteralToComparisonFunc(this->mAST[node.States[EffectNodes::Pass::DepthFunc]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_COMPARISON_LESS;
				ddesc.StencilEnable = node.States[EffectNodes::Pass::StencilEnable] != 0 && this->mAST[node.States[EffectNodes::Pass::StencilEnable]].As<EffectNodes::Literal>().Value.Bool[0];
				ddesc.StencilReadMask = (node.States[EffectNodes::Pass::StencilReadMask] != 0) ? static_cast<UINT8>(this->mAST[node.States[EffectNodes::Pass::StencilReadMask]].As<EffectNodes::Literal>().Value.Uint[0] & 0xFF) : D3D11_DEFAULT_STENCIL_READ_MASK;
				ddesc.StencilWriteMask = (node.States[EffectNodes::Pass::StencilWriteMask] != 0) != 0 ? static_cast<UINT8>(this->mAST[node.States[EffectNodes::Pass::StencilWriteMask]].As<EffectNodes::Literal>().Value.Uint[0] & 0xFF) : D3D11_DEFAULT_STENCIL_WRITE_MASK;
				ddesc.FrontFace.StencilFunc = ddesc.BackFace.StencilFunc = (node.States[EffectNodes::Pass::StencilFunc] != 0) ? LiteralToComparisonFunc(this->mAST[node.States[EffectNodes::Pass::StencilFunc]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_COMPARISON_ALWAYS;
				ddesc.FrontFace.StencilPassOp = ddesc.BackFace.StencilPassOp = (node.States[EffectNodes::Pass::StencilPass] != 0) ? LiteralToStencilOp(this->mAST[node.States[EffectNodes::Pass::StencilPass]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_STENCIL_OP_KEEP;
				ddesc.FrontFace.StencilFailOp = ddesc.BackFace.StencilFailOp = (node.States[EffectNodes::Pass::StencilFail] != 0) ? LiteralToStencilOp(this->mAST[node.States[EffectNodes::Pass::StencilFail]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_STENCIL_OP_KEEP;
				ddesc.FrontFace.StencilDepthFailOp = ddesc.BackFace.StencilDepthFailOp = (node.States[EffectNodes::Pass::StencilDepthFail] != 0) ? LiteralToStencilOp(this->mAST[node.States[EffectNodes::Pass::StencilDepthFail]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_STENCIL_OP_KEEP;

				if (node.States[EffectNodes::Pass::StencilRef] != 0)
				{
					pass.StencilRef = this->mAST[node.States[EffectNodes::Pass::StencilRef]].As<EffectNodes::Literal>().Value.Uint[0];
				}

				HRESULT hr = this->mEffect->mRuntime->mDevice->CreateDepthStencilState(&ddesc, &pass.DSS);

				if (FAILED(hr))
				{
					this->mErrors += PrintLocation(node.Location) + "warning: 'CreateDepthStencilState' failed!\n";
				}

				D3D11_BLEND_DESC bdesc;
				bdesc.AlphaToCoverageEnable = FALSE;
				bdesc.IndependentBlendEnable = FALSE;
				bdesc.RenderTarget[0].RenderTargetWriteMask = (node.States[EffectNodes::Pass::ColorWriteMask] != 0) ? static_cast<UINT8>(this->mAST[node.States[EffectNodes::Pass::ColorWriteMask]].As<EffectNodes::Literal>().Value.Uint[0] & 0xF) : D3D11_COLOR_WRITE_ENABLE_ALL;
				bdesc.RenderTarget[0].BlendEnable = node.States[EffectNodes::Pass::BlendEnable] != 0 && this->mAST[node.States[EffectNodes::Pass::BlendEnable]].As<EffectNodes::Literal>().Value.Bool[0];
				bdesc.RenderTarget[0].BlendOp = (node.States[EffectNodes::Pass::BlendOp] != 0) ? LiteralToBlendOp(this->mAST[node.States[EffectNodes::Pass::BlendOp]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_BLEND_OP_ADD;
				bdesc.RenderTarget[0].BlendOpAlpha = (node.States[EffectNodes::Pass::BlendOpAlpha] != 0) ? LiteralToBlendOp(this->mAST[node.States[EffectNodes::Pass::BlendOpAlpha]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_BLEND_OP_ADD;
				bdesc.RenderTarget[0].SrcBlend = (node.States[EffectNodes::Pass::SrcBlend] != 0) ? LiteralToBlend(this->mAST[node.States[EffectNodes::Pass::SrcBlend]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_BLEND_ONE;
				bdesc.RenderTarget[0].DestBlend = (node.States[EffectNodes::Pass::DestBlend] != 0) ? LiteralToBlend(this->mAST[node.States[EffectNodes::Pass::DestBlend]].As<EffectNodes::Literal>().Value.Uint[0]) : D3D11_BLEND_ZERO;

				hr = this->mEffect->mRuntime->mDevice->CreateBlendState(&bdesc, &pass.BS);

				if (FAILED(hr))
				{
					this->mErrors += PrintLocation(node.Location) + "warning: 'CreateBlendState' failed!\n";
				}

				for (ID3D11ShaderResourceView *&srv : pass.SRV)
				{
					if (srv == nullptr)
					{
						continue;
					}

					ID3D11Resource *res1, *res2;
					srv->GetResource(&res1);
					res1->Release();

					for (ID3D11RenderTargetView *rtv : pass.RT)
					{
						if (rtv == nullptr)
						{
							continue;
						}

						rtv->GetResource(&res2);
						res2->Release();

						if (res1 == res2)
						{
							srv = nullptr;
							break;
						}
					}
				}

				passes.push_back(std::move(pass));
			}
			void VisitShader(const EffectNodes::Function &node, unsigned int shadertype, D3D11Technique::Pass &pass)
			{
				std::string profile;

				switch (this->mEffect->mRuntime->mDevice->GetFeatureLevel())
				{
					default:
					case D3D_FEATURE_LEVEL_11_0:
						profile = "5_0";
						break;
					case D3D_FEATURE_LEVEL_10_1:
						profile = "4_1";
						break;
					case D3D_FEATURE_LEVEL_10_0:
						profile = "4_0";
						break;
					case D3D_FEATURE_LEVEL_9_1:
					case D3D_FEATURE_LEVEL_9_2:
						profile = "4_0_level_9_1";
						break;
					case D3D_FEATURE_LEVEL_9_3:
						profile = "4_0_level_9_3";
						break;
				}

				switch (shadertype)
				{
					default:
						return;
					case EffectNodes::Pass::VertexShader:
						profile = "vs_" + profile;
						break;
					case EffectNodes::Pass::PixelShader:
						profile = "ps_" + profile;
						break;
				}

				UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
				flags |= D3DCOMPILE_DEBUG;
#endif

				std::string source =
					"struct __sampler2D { Texture2D t; SamplerState s; };\n"
					"inline float4 __tex2D(__sampler2D s, float2 c) { return s.t.Sample(s.s, c); }\n"
					"inline float4 __tex2Doffset(__sampler2D s, float2 c, int2 offset) { return s.t.Sample(s.s, c, offset); }\n"
					"inline float4 __tex2Dlod(__sampler2D s, float4 c) { return s.t.SampleLevel(s.s, c.xy, c.w); }\n"
					"inline float4 __tex2Dlodoffset(__sampler2D s, float4 c, int2 offset) { return s.t.SampleLevel(s.s, c.xy, c.w, offset); }\n"
					"inline float4 __tex2Dgather(__sampler2D s, float2 c) { return s.t.Gather(s.s, c); }\n"
					"inline float4 __tex2Dgatheroffset(__sampler2D s, float2 c, int2 offset) { return s.t.Gather(s.s, c, offset); }\n"
					"inline float4 __tex2Dfetch(__sampler2D s, int4 c) { return s.t.Load(c.xyw); }\n"
					"inline float4 __tex2Dbias(__sampler2D s, float4 c) { return s.t.SampleBias(s.s, c.xy, c.w); }\n"
					"inline int2 __tex2Dsize(__sampler2D s, int lod) { uint w, h, l; s.t.GetDimensions(lod, w, h, l); return int2(w, h); }\n";

				if (!this->mCurrentGlobalConstants.empty())
				{
					source += "cbuffer __GLOBAL__ : register(b0)\n{\n" + this->mCurrentGlobalConstants + "};\n";
				}

				source += this->mCurrentSource;

				LOG(TRACE) << "> Compiling shader '" << node.Name << "':\n\n" << source.c_str() << "\n";

				ID3DBlob *compiled = nullptr, *errors = nullptr;

				HRESULT hr = D3DCompile(source.c_str(), source.length(), nullptr, nullptr, nullptr, node.Name, profile.c_str(), flags, 0, &compiled, &errors);

				if (errors != nullptr)
				{
					this->mErrors += std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());

					errors->Release();
				}

				if (FAILED(hr))
				{
					this->mFatal = true;
					return;
				}

				switch (shadertype)
				{
					case EffectNodes::Pass::VertexShader:
						hr = this->mEffect->mRuntime->mDevice->CreateVertexShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, &pass.VS);
						break;
					case EffectNodes::Pass::PixelShader:
						hr = this->mEffect->mRuntime->mDevice->CreatePixelShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), nullptr, &pass.PS);
						break;
				}

				compiled->Release();

				if (FAILED(hr))
				{
					this->mErrors += PrintLocation(node.Location) + "error: 'CreateShader' failed!\n";
					this->mFatal = true;
					return;
				}
			}

		private:
			const EffectTree &mAST;
			D3D11Effect *mEffect;
			std::string mCurrentSource;
			std::string mErrors;
			bool mFatal;
			std::unordered_map<std::size_t, std::size_t> mSamplerDescs;
			std::string mCurrentGlobalConstants;
			UINT mCurrentGlobalSize, mCurrentGlobalStorageSize;
			std::string mCurrentBlockName;
			bool mCurrentInParameterBlock, mCurrentInFunctionBlock, mCurrentInDeclaratorList;
		};

		template <typename T>
		inline ULONG SAFE_RELEASE(T *&object)
		{
			if (object == nullptr)
			{
				return 0;
			}

			const ULONG ref = object->Release();

			object = nullptr;

			return ref;
		}
	}
	
	class D3D11StateBlock
	{
	public:
		D3D11StateBlock(ID3D11Device *device)
		{
			ZeroMemory(this, sizeof(D3D11StateBlock));

			device->GetImmediateContext(&this->mDeviceContext);
			this->mFeatureLevel = device->GetFeatureLevel();
		}
		~D3D11StateBlock()
		{
			ReleaseAllDeviceObjects();

			this->mDeviceContext->Release();
		}

		void Capture()
		{
			ReleaseAllDeviceObjects();

			this->mDeviceContext->IAGetPrimitiveTopology(&this->mIAPrimitiveTopology);
			this->mDeviceContext->IAGetInputLayout(&this->mIAInputLayout);

			if (this->mFeatureLevel > D3D_FEATURE_LEVEL_10_0)
			{
				this->mDeviceContext->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, this->mIAVertexBuffers, this->mIAVertexStrides, this->mIAVertexOffsets);
			}
			else
			{
				this->mDeviceContext->IAGetVertexBuffers(0, D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, this->mIAVertexBuffers, this->mIAVertexStrides, this->mIAVertexOffsets);
			}

			this->mDeviceContext->IAGetIndexBuffer(&this->mIAIndexBuffer, &this->mIAIndexFormat, &this->mIAIndexOffset);

			this->mDeviceContext->RSGetState(&this->mRSState);
			this->mDeviceContext->RSGetViewports(&this->mRSNumViewports, this->mRSViewports);

			this->mVSNumClassInstances = 256;
			this->mDeviceContext->VSGetShader(&this->mVS, this->mVSClassInstances, &this->mVSNumClassInstances);
			this->mDeviceContext->VSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, this->mVSConstantBuffers);
			this->mDeviceContext->VSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, this->mVSSamplerStates);
			this->mDeviceContext->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, this->mVSShaderResources);

			if (this->mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
			{
				if (this->mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
				{
					this->mHSNumClassInstances = 256;
					this->mDeviceContext->HSGetShader(&this->mHS, this->mHSClassInstances, &this->mHSNumClassInstances);

					this->mDSNumClassInstances = 256;
					this->mDeviceContext->DSGetShader(&this->mDS, this->mDSClassInstances, &this->mDSNumClassInstances);
				}

				this->mGSNumClassInstances = 256;
				this->mDeviceContext->GSGetShader(&this->mGS, this->mGSClassInstances, &this->mGSNumClassInstances);
			}

			this->mPSNumClassInstances = 256;
			this->mDeviceContext->PSGetShader(&this->mPS, this->mPSClassInstances, &this->mPSNumClassInstances);
			this->mDeviceContext->PSGetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, this->mPSConstantBuffers);
			this->mDeviceContext->PSGetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, this->mPSSamplerStates);
			this->mDeviceContext->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, this->mPSShaderResources);

			this->mDeviceContext->OMGetBlendState(&this->mOMBlendState, this->mOMBlendFactor, &this->mOMSampleMask);
			this->mDeviceContext->OMGetDepthStencilState(&this->mOMDepthStencilState, &this->mOMStencilRef);
			this->mDeviceContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, this->mOMRenderTargets, &this->mOMDepthStencil);
		}
		void Apply()
		{
			this->mDeviceContext->IASetPrimitiveTopology(this->mIAPrimitiveTopology);
			this->mDeviceContext->IASetInputLayout(this->mIAInputLayout);

			if (this->mFeatureLevel > D3D_FEATURE_LEVEL_10_0)
			{
				this->mDeviceContext->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, this->mIAVertexBuffers, this->mIAVertexStrides, this->mIAVertexOffsets);
			}
			else
			{
				this->mDeviceContext->IASetVertexBuffers(0, D3D10_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, this->mIAVertexBuffers, this->mIAVertexStrides, this->mIAVertexOffsets);
			}

			this->mDeviceContext->IASetIndexBuffer(this->mIAIndexBuffer, this->mIAIndexFormat, this->mIAIndexOffset);

			this->mDeviceContext->RSSetState(this->mRSState);
			this->mDeviceContext->RSSetViewports(this->mRSNumViewports, this->mRSViewports);

			this->mDeviceContext->VSSetShader(this->mVS, this->mVSClassInstances, this->mVSNumClassInstances);
			this->mDeviceContext->VSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, this->mVSConstantBuffers);
			this->mDeviceContext->VSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, this->mVSSamplerStates);
			this->mDeviceContext->VSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, this->mVSShaderResources);

			if (this->mFeatureLevel >= D3D_FEATURE_LEVEL_10_0)
			{
				if (this->mFeatureLevel >= D3D_FEATURE_LEVEL_11_0)
				{
					this->mDeviceContext->HSSetShader(this->mHS, this->mHSClassInstances, this->mHSNumClassInstances);

					this->mDeviceContext->DSSetShader(this->mDS, this->mDSClassInstances, this->mDSNumClassInstances);
				}

				this->mDeviceContext->GSSetShader(this->mGS, this->mGSClassInstances, this->mGSNumClassInstances);
			}

			this->mDeviceContext->PSSetShader(this->mPS, this->mPSClassInstances, this->mPSNumClassInstances);
			this->mDeviceContext->PSSetConstantBuffers(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, this->mPSConstantBuffers);
			this->mDeviceContext->PSSetSamplers(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, this->mPSSamplerStates);
			this->mDeviceContext->PSSetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, this->mPSShaderResources);

			this->mDeviceContext->OMSetBlendState(this->mOMBlendState, this->mOMBlendFactor, this->mOMSampleMask);
			this->mDeviceContext->OMSetDepthStencilState(this->mOMDepthStencilState, this->mOMStencilRef);
			this->mDeviceContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, this->mOMRenderTargets, this->mOMDepthStencil);
		}
		void ReleaseAllDeviceObjects()
		{
			SAFE_RELEASE(this->mIAInputLayout);

			for (UINT i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mIAVertexBuffers[i]);
			}

			SAFE_RELEASE(this->mIAIndexBuffer);

			SAFE_RELEASE(this->mVS);

			for (UINT i = 0; i < this->mVSNumClassInstances; ++i)
			{
				SAFE_RELEASE(this->mVSClassInstances[i]);
			}
			for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mVSConstantBuffers[i]);
			}
			for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mVSSamplerStates[i]);
			}
			for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mVSShaderResources[i]);
			}

			SAFE_RELEASE(this->mHS);

			for (UINT i = 0; i < this->mHSNumClassInstances; ++i)
			{
				SAFE_RELEASE(this->mHSClassInstances[i]);
			}

			SAFE_RELEASE(this->mDS);
			
			for (UINT i = 0; i < this->mDSNumClassInstances; ++i)
			{
				SAFE_RELEASE(this->mDSClassInstances[i]);
			}

			SAFE_RELEASE(this->mGS);

			for (UINT i = 0; i < this->mGSNumClassInstances; ++i)
			{
				SAFE_RELEASE(this->mGSClassInstances[i]);
			}

			SAFE_RELEASE(this->mRSState);

			SAFE_RELEASE(this->mPS);
			
			for (UINT i = 0; i < this->mPSNumClassInstances; ++i)
			{
				SAFE_RELEASE(this->mPSClassInstances[i]);
			}
			for (UINT i = 0; i < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mPSConstantBuffers[i]);
			}
			for (UINT i = 0; i < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mPSSamplerStates[i]);
			}
			for (UINT i = 0; i < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++i)
			{
				SAFE_RELEASE(this->mPSShaderResources[i]);
			}

			SAFE_RELEASE(this->mOMBlendState);
			SAFE_RELEASE(this->mOMDepthStencilState);

			for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
			{
				SAFE_RELEASE(this->mOMRenderTargets[i]);
			}

			SAFE_RELEASE(this->mOMDepthStencil);
		}

	private:
		ID3D11DeviceContext *mDeviceContext;
		D3D_FEATURE_LEVEL mFeatureLevel;

		ID3D11InputLayout *mIAInputLayout;
		D3D11_PRIMITIVE_TOPOLOGY mIAPrimitiveTopology;
		ID3D11Buffer *mIAVertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
		UINT mIAVertexStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
		UINT mIAVertexOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
		ID3D11Buffer *mIAIndexBuffer;
		DXGI_FORMAT mIAIndexFormat;
		UINT mIAIndexOffset;
		ID3D11VertexShader *mVS;
		UINT mVSNumClassInstances;
		ID3D11ClassInstance *mVSClassInstances[256];
		ID3D11Buffer *mVSConstantBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
		ID3D11SamplerState *mVSSamplerStates[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
		ID3D11ShaderResourceView *mVSShaderResources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
		ID3D11HullShader *mHS;
		UINT mHSNumClassInstances;
		ID3D11ClassInstance *mHSClassInstances[256];
		ID3D11DomainShader *mDS;
		UINT mDSNumClassInstances;
		ID3D11ClassInstance *mDSClassInstances[256];
		ID3D11GeometryShader *mGS;
		UINT mGSNumClassInstances;
		ID3D11ClassInstance *mGSClassInstances[256];
		ID3D11RasterizerState *mRSState;
		UINT mRSNumViewports;
		D3D11_VIEWPORT mRSViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		ID3D11PixelShader *mPS;
		UINT mPSNumClassInstances;
		ID3D11ClassInstance *mPSClassInstances[256];
		ID3D11Buffer *mPSConstantBuffers[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT];
		ID3D11SamplerState *mPSSamplerStates[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
		ID3D11ShaderResourceView *mPSShaderResources[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
		ID3D11BlendState *mOMBlendState;
		FLOAT mOMBlendFactor[4];
		UINT mOMSampleMask;
		ID3D11DepthStencilState *mOMDepthStencilState;
		UINT mOMStencilRef;
		ID3D11RenderTargetView *mOMRenderTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
		ID3D11DepthStencilView *mOMDepthStencil;
	};

	// -----------------------------------------------------------------------------------------------------

	D3D11Runtime::D3D11Runtime(ID3D11Device *device, IDXGISwapChain *swapchain) : mDevice(device), mSwapChain(swapchain), mImmediateContext(nullptr), mStateBlock(new D3D11StateBlock(device)), mBackBuffer(nullptr), mBackBufferReplacement(nullptr), mBackBufferTexture(nullptr), mBackBufferTextureSRV(), mBackBufferTargets(), mDepthStencil(nullptr), 
		mDepthStencilReplacement(nullptr), mDepthStencilTexture(nullptr), mDepthStencilTextureSRV(nullptr), 
		mLost(true), lastMRTsCount(0)
	{
		mIsDumpingTrace = false;
		mToggleDebugViewID = -1;
		InitializeCriticalSection(&this->mCS);

		assert(this->mDevice != nullptr);
		assert(this->mSwapChain != nullptr);
		assert(this->mStateBlock != nullptr);

		this->mDevice->AddRef();
		this->mDevice->GetImmediateContext(&this->mImmediateContext);
		this->mSwapChain->AddRef();

		assert(this->mImmediateContext != nullptr);

		ZeroMemory(&this->mSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

		IDXGIDevice *dxgidevice = nullptr;
		IDXGIAdapter *adapter = nullptr;

		HRESULT hr = this->mDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgidevice));

		assert(SUCCEEDED(hr));

		hr = dxgidevice->GetAdapter(&adapter);

		SAFE_RELEASE(dxgidevice);

		assert(SUCCEEDED(hr));

		DXGI_ADAPTER_DESC desc;
		hr = adapter->GetDesc(&desc);

		SAFE_RELEASE(adapter);

		assert(SUCCEEDED(hr));

		this->mVendorId = desc.VendorId;
		this->mDeviceId = desc.DeviceId;
		this->mRendererId = 0xD3D11;
	}
	D3D11Runtime::~D3D11Runtime()
	{
		DeleteCriticalSection(&this->mCS);

		assert(this->mLost);

		this->mImmediateContext->Release();
		this->mDevice->Release();
		this->mSwapChain->Release();
	}

	bool D3D11Runtime::OnCreateInternal(const DXGI_SWAP_CHAIN_DESC &desc)
	{
		this->mSwapChainDesc = desc;

		HRESULT hr = this->mSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&this->mBackBuffer));

		// This called several times by MGS, with different resolutions
		// and RTs are created prior to the resize
		// BackbufferWidth = desc.BufferDesc.Width;
		// BackbufferHeight = desc.BufferDesc.Height;

		assert(SUCCEEDED(hr));

		D3D11_TEXTURE2D_DESC texdesc;
		this->mBackBuffer->GetDesc(&texdesc);

		LOG(INFO) << "Back buffer replacement. Format  " << texdesc.Format;

		if (!CreateBackBufferReplacement(this->mBackBuffer, desc.SampleDesc))
		{
			LOG(TRACE) << "Failed to create backbuffer replacement!";

			SAFE_RELEASE(this->mBackBuffer);

			return false;
		}

		D3D11_TEXTURE2D_DESC dstdesc;
		ZeroMemory(&dstdesc, sizeof(D3D11_TEXTURE2D_DESC));
		dstdesc.Width = desc.BufferDesc.Width;
		dstdesc.Height = desc.BufferDesc.Height;
		dstdesc.MipLevels = 1;
		dstdesc.ArraySize = 1;
		dstdesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dstdesc.SampleDesc.Count = 1;
		dstdesc.SampleDesc.Quality = 0;
		dstdesc.Usage = D3D11_USAGE_DEFAULT;
		dstdesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		ID3D11Texture2D *dstexture = nullptr;
		hr = this->mDevice->CreateTexture2D(&dstdesc, nullptr, &dstexture);

		if (SUCCEEDED(hr))
		{
			hr = this->mDevice->CreateDepthStencilView(dstexture, nullptr, &this->mDefaultDepthStencil);

			SAFE_RELEASE(dstexture);
		}
		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create default depthstencil! HRESULT is '" << hr << "'.";

			SAFE_RELEASE(this->mBackBuffer);
			SAFE_RELEASE(this->mBackBufferReplacement);
			SAFE_RELEASE(this->mBackBufferTexture);
			SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
			SAFE_RELEASE(this->mBackBufferTextureSRV[1]);
			SAFE_RELEASE(this->mBackBufferTargets[0]);
			SAFE_RELEASE(this->mBackBufferTargets[1]);

			return false;
		}

		this->mNVG = nvgCreateD3D11(this->mDevice, 0);

		this->mLost = false;

		Runtime::OnCreate(desc.BufferDesc.Width, desc.BufferDesc.Height);

		return true;
	}
	void D3D11Runtime::OnDeleteInternal()
	{
		Runtime::OnDelete();

		nvgDeleteD3D11(this->mNVG);

		this->mNVG = nullptr;

		this->mStateBlock->ReleaseAllDeviceObjects();

		SAFE_RELEASE(this->mBackBuffer);
		SAFE_RELEASE(this->mBackBufferReplacement);
		SAFE_RELEASE(this->mBackBufferTexture);
		SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
		SAFE_RELEASE(this->mBackBufferTextureSRV[1]);
		SAFE_RELEASE(this->mBackBufferTargets[0]);
		SAFE_RELEASE(this->mBackBufferTargets[1]);

		SAFE_RELEASE(this->mDepthStencil);
		SAFE_RELEASE(this->mDepthStencilReplacement);
		SAFE_RELEASE(this->mDepthStencilTexture);
		SAFE_RELEASE(this->mDepthStencilTextureSRV);

		SAFE_RELEASE(this->mDefaultDepthStencil);

		ZeroMemory(&this->mSwapChainDesc, sizeof(DXGI_SWAP_CHAIN_DESC));

		this->mLost = true;
	}

	void D3D11Runtime::OnDrawInternal(ID3D11DeviceContext *context, unsigned int vertices)
	{
		CSLock lock(this->mCS);

		Runtime::OnDraw(vertices);

		//LOG(INFO) << "Draw: " << vertices;

		ID3D11DepthStencilView *depthstencil = nullptr;
		//context->OMGetRenderTargets(0, nullptr, &depthstencil);

		if (depthstencil != nullptr)
		{
			depthstencil->Release();

			if (depthstencil == this->mDefaultDepthStencil)
			{
				return;
			}
			else if (depthstencil == this->mDepthStencilReplacement)
			{
				depthstencil = this->mDepthStencil;
			}

			const auto it = this->mDepthSourceTable.find(depthstencil);

			if (it != this->mDepthSourceTable.end())
			{
				it->second.DrawCallCount = static_cast<FLOAT>(this->mLastDrawCalls);
				it->second.DrawVerticesCount += vertices;
			}
		}
	}

	void D3D11Runtime::OnExecuteCommandList(ID3D11DeviceContext *context, ID3D11CommandList *pCommandList, BOOL RestoreContextState) {
		
		static int drawCounter = 0;
		
		drawCounter++;

		if ( mIsDumpingTrace) {
			LOG(INFO) << "EXEC CL";

			char drawEventString[260];
			snprintf(drawEventString, 260, "%05d_Draw", drawCounter);

			ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
			ID3D11DepthStencilView* depthstencil = nullptr;

			// This doesn't work, always return 0 MRT bound because we're in a deferred context
			//this->mImmediateContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthstencil);
			
			bool depthPresent = false;
			if (depthstencil != nullptr)
			{
				depthPresent = true;

				depthstencil->Release();
			}

			int RTCount = 0;

			for (auto RT : WatchedRTs)
			{
				if (RT) {
					//LOG(INFO) << "Found RT " << i;
					RTCount++;

					ID3D11Resource* resource;
					RT->GetResource(&resource);

					D3D11_RENDER_TARGET_VIEW_DESC texdesc;
					RT->GetDesc(&texdesc);

					ID3D11Texture2D *texture = nullptr;
					HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));

					if (FAILED(hr))
					{
						LOG(INFO) << "Was not a texture 2D!";
						continue;
					}

					DumpTexture2D( DumpRootPath / drawEventString / (std::string("RT") + std::to_string(RTCount - 1)),  texture);

					SAFE_RELEASE(texture);

				}
				//SAFE_RELEASE(targets[i]);
			}

			LOG(INFO) << "Begin CL RT#: " << RTCount << ". Depth:" << ((depthPresent) ? "1" : "0");
		}
		else {
			drawCounter = 0;
		}
	}

	void D3D11Runtime::DumpReadableTexture2D(boost::filesystem::path rootPathNoExt, ID3D11Texture2D* texture)
	{
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);

		LOG(INFO) << "Texture info: " << desc.Width << "x" << desc.Height << " format: " << desc.Format << " Dumping to: " << rootPathNoExt.string();

		ID3D11Texture2D* textureStaging = texture;
		D3D11_MAPPED_SUBRESOURCE mapped;
		HRESULT hr = this->mImmediateContext->Map(textureStaging, 0, D3D11_MAP_READ, 0, &mapped);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to map staging resource for texture dump! HRESULT is '" << hr << "'.";
			return;
		}

		BYTE *pMapped = static_cast<BYTE *>(mapped.pData);

		// Time to save to disk
		if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM
			|| desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
			|| desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
			|| desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
			|| desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)

		{
			const std::size_t dataSize = desc.Width * desc.Height * 4;

			if (dataSize > 0)
			{
				unsigned char* pMem = new unsigned char[dataSize];
				::memset(pMem, 0, dataSize);

				int* data = (int*)mapped.pData;

				unsigned char* pMemPoint = pMem;
				unsigned char* pMappedPoint = pMapped;

				// Will save as LDR PNG file
				const UINT pitch = desc.Width * 4;

				for (UINT y = 0; y < desc.Height; ++y)
				{
					CopyMemory(pMemPoint, pMappedPoint, std::min(pitch, static_cast<UINT>(mapped.RowPitch)));

					for (UINT x = 0; x < pitch; x += 4)
					{
						pMemPoint[x + 3] = 0xFF;

						if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM
							|| desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
							|| desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
						{
							std::swap(pMemPoint[x + 0], pMemPoint[x + 2]);
						}
					}

					pMemPoint += pitch;
					pMappedPoint += mapped.RowPitch;
				}


				boost::filesystem::path imagePath = rootPathNoExt.string() + ".png";
				//Make sure parent folder exists
				boost::system::error_code returnedError;
				boost::filesystem::create_directories(imagePath.parent_path(), returnedError);
				//LOG(INFO) << "Dumping image to: " << imagePath.string();
				//LOG(INFO) << "Width: " << desc.Width << " Height: " << desc.Height;
				// Dump to disk
				if (!stbi_write_png(imagePath.string().c_str(), desc.Width, desc.Height, 4, pMem, 0))
				{
					LOG(ERROR) << "Failed to write screenshot to " << imagePath.string() << "!";
				}

				delete[] pMem;
			}

		}

		this->mImmediateContext->Unmap(textureStaging, 0);
	}

	void D3D11Runtime::DumpTexture2D(boost::filesystem::path rootPathNoExt,ID3D11Texture2D* texture)
	{
		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);

		//-
		D3D11_TEXTURE2D_DESC copydesc;
		ZeroMemory(&copydesc, sizeof(D3D11_TEXTURE2D_DESC));
		copydesc.Width = desc.Width;
		copydesc.Height = desc.Height;
		copydesc.ArraySize = 1;
		copydesc.MipLevels = 1;
		copydesc.Format = desc.Format;
		copydesc.SampleDesc.Count = 1;
		copydesc.SampleDesc.Quality = 0;
		copydesc.Usage = D3D11_USAGE_STAGING;
		copydesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		ID3D11Texture2D *textureStaging = nullptr;
		HRESULT hr = this->mDevice->CreateTexture2D(&copydesc, nullptr, &textureStaging); 

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create staging resource for texture dump! HRESULT is '" << hr << "'.";
			return;
		}

		this->mImmediateContext->CopyResource(textureStaging, texture);

		DumpReadableTexture2D(rootPathNoExt, textureStaging);

		textureStaging->Release();
	}

	void D3D11Runtime::OnSetMRT(UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView) 
	{
		//if (mIsDumpingTrace) LOG(INFO) << "MRT Count: " << NumViews;
		lastMRTsCount = NumViews;
		for (int i = 0; i < NumViews; i++) 
		{
			lastMRTs[i] = ppRenderTargetViews[i];
			//if (mIsDumpingTrace) LOG(INFO) << "View" << i << " " << lastMRTs[i];
		}
		lastDepthStencil = pDepthStencilView;
	}

	static int swapBufferCount = 0;

	void D3D11Runtime::OnPresentInternal()
	{
		if (mIsDumpingTrace)
		{
			LOG(INFO) << "Trace dump completed.";
			swapBufferCount--;
			if (swapBufferCount == 0) mIsDumpingTrace = false;
		}

		if (this->mLost)
		{
			LOG(TRACE) << "Failed to present! Runtime is in a lost state.";
			return;
		}

		DetectDepthSource();

		// Capture device state
		this->mStateBlock->Capture();

		// Resolve backbuffer
		if (this->mBackBufferReplacement != this->mBackBuffer)
		{
			this->mImmediateContext->ResolveSubresource(this->mBackBuffer, 0, this->mBackBufferReplacement, 0, this->mSwapChainDesc.BufferDesc.Format);
		}

		// Setup real backbuffer
		this->mImmediateContext->OMSetRenderTargets(1, &this->mBackBufferTargets[0], nullptr);

		// Apply post processing
		Runtime::OnPostProcess();

		// Reset rendertargets
		this->mImmediateContext->OMSetRenderTargets(1, &this->mBackBufferTargets[0], this->mDefaultDepthStencil);
		
		const D3D11_VIEWPORT viewport = { 0, 0, static_cast<FLOAT>(this->mSwapChainDesc.BufferDesc.Width), static_cast<FLOAT>(this->mSwapChainDesc.BufferDesc.Height), 0.0f, 1.0f };
		this->mImmediateContext->RSSetViewports(1, &viewport);

		// Apply presenting
		Runtime::OnPresent();

		if (this->mLost)
		{
			return;
		}

		// Apply previous device state
		this->mStateBlock->Apply();
	}
	void D3D11Runtime::OnGetBackBuffer(ID3D11Texture2D *&buffer)
	{
		if (this->mBackBufferReplacement != nullptr)
		{
			buffer = this->mBackBufferReplacement;
		}
	}
	void D3D11Runtime::OnCreateDepthStencilView(ID3D11Resource *resource, ID3D11DepthStencilView *depthstencil)
	{
		assert(resource != nullptr);
		assert(depthstencil != nullptr);

		CSLock lock(this->mCS);

		// Do not track default depthstencil
		if (this->mLost)
		{
			return;
		}

		ID3D11Texture2D *texture = nullptr;
		const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));

		if (FAILED(hr))
		{
			return;
		}

		D3D11_TEXTURE2D_DESC desc;
		texture->GetDesc(&desc);

		SAFE_RELEASE(texture);

		// Early depthstencil rejection
		if (desc.Width != this->mSwapChainDesc.BufferDesc.Width || desc.Height != this->mSwapChainDesc.BufferDesc.Height || desc.SampleDesc.Count > 1)
		{
			return;
		}

		LOG(TRACE) << "Adding depthstencil " << depthstencil << " (Width: " << desc.Width << ", Height: " << desc.Height << ", Format: " << desc.Format << ") to list of possible depth candidates ...";

		// Begin tracking new depthstencil
		const DepthSourceInfo info = { desc.Width, desc.Height };
		this->mDepthSourceTable.emplace(depthstencil, info);
	}
	void D3D11Runtime::OnDeleteDepthStencilView(ID3D11DepthStencilView *depthstencil)
	{
		assert(depthstencil != nullptr);

		CSLock lock(this->mCS);

		const auto it = this->mDepthSourceTable.find(depthstencil);

		if (it != this->mDepthSourceTable.end())
		{
			LOG(TRACE) << "Removing depthstencil " << depthstencil << " from list of possible depth candidates ...";

			this->mDepthSourceTable.erase(it);
		}
	}
	void D3D11Runtime::OnSetDepthStencilView(ID3D11DepthStencilView *&depthstencil)
	{
		CSLock lock(this->mCS);

		if (this->mDepthStencilReplacement != nullptr && depthstencil == this->mDepthStencil)
		{
			depthstencil = this->mDepthStencilReplacement;
		}
	}
	void D3D11Runtime::OnGetDepthStencilView(ID3D11DepthStencilView *&depthstencil)
	{
		CSLock lock(this->mCS);

		if (this->mDepthStencilReplacement != nullptr && depthstencil == this->mDepthStencilReplacement)
		{
			depthstencil = this->mDepthStencil;
		}
	}
	void D3D11Runtime::OnClearDepthStencilView(ID3D11DepthStencilView *&depthstencil)
	{
		CSLock lock(this->mCS);

		if (this->mDepthStencilReplacement != nullptr && depthstencil == this->mDepthStencil)
		{
			depthstencil = this->mDepthStencilReplacement;
		}
	}
	void D3D11Runtime::OnCopyResource(ID3D11Resource *&dest, ID3D11Resource *&source)
	{
		CSLock lock(this->mCS);

		if (this->mDepthStencilReplacement != nullptr)
		{
			ID3D11Resource *resource = nullptr;
			this->mDepthStencil->GetResource(&resource);

			if (dest == resource)
			{
				dest = this->mDepthStencilTexture;
			}
			if (source == resource)
			{
				source = this->mDepthStencilTexture;
			}

			resource->Release();
		}
	}

	void D3D11Runtime::DetectDepthSource()
	{
		static int cooldown = 0, traffic = 0;

		if (cooldown-- > 0)
		{
			traffic += (sNetworkUpload + sNetworkDownload) > 0;
			return;
		}
		else
		{
			cooldown = 30;

			if (traffic > 10)
			{
				traffic = 0;
				CreateDepthStencilReplacement(nullptr);
				return;
			}
			else
			{
				traffic = 0;
			}
		}

		CSLock lock(this->mCS);

		if (this->mSwapChainDesc.SampleDesc.Count > 1 || this->mDepthSourceTable.empty())
		{
			return;
		}

		DepthSourceInfo bestInfo = { 0 };
		ID3D11DepthStencilView *best = nullptr;

		for (auto &it : this->mDepthSourceTable)
		{
			if (it.second.DrawCallCount == 0)
			{
				continue;
			}
			else if ((it.second.DrawVerticesCount * (1.2f - it.second.DrawCallCount / this->mLastDrawCalls)) >= (bestInfo.DrawVerticesCount * (1.2f - bestInfo.DrawCallCount / this->mLastDrawCalls)))
			{
				best = it.first;
				bestInfo = it.second;
			}

			it.second.DrawCallCount = it.second.DrawVerticesCount = 0;
		}

		if (best != nullptr && this->mDepthStencil != best)
		{
			LOG(TRACE) << "Switched depth source to depthstencil " << best << ".";

			CreateDepthStencilReplacement(best);
		}
	}
	bool D3D11Runtime::CreateBackBufferReplacement(ID3D11Texture2D *backbuffer, const DXGI_SAMPLE_DESC &samples)
	{
		D3D11_TEXTURE2D_DESC texdesc;
		backbuffer->GetDesc(&texdesc);

		HRESULT hr;

		texdesc.SampleDesc = samples;
		texdesc.BindFlags = D3D11_BIND_RENDER_TARGET;

		if (samples.Count > 1)
		{
			hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &this->mBackBufferReplacement);

			if (FAILED(hr))
			{
				return false;
			}
		}
		else
		{
			this->mBackBufferReplacement = this->mBackBuffer;
			this->mBackBufferReplacement->AddRef();
		}

		texdesc.Format = D3D11EffectCompiler::MakeTypelessFormat(texdesc.Format);
		texdesc.SampleDesc.Count = 1;
		texdesc.SampleDesc.Quality = 0;
		texdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &this->mBackBufferTexture);

		if (SUCCEEDED(hr))
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc;
			ZeroMemory(&srvdesc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
			srvdesc.Format = D3D11EffectCompiler::MakeNonSRBFormat(texdesc.Format);	
			srvdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvdesc.Texture2D.MipLevels = texdesc.MipLevels;

			if (SUCCEEDED(hr))
			{
				hr = this->mDevice->CreateShaderResourceView(this->mBackBufferTexture, &srvdesc, &this->mBackBufferTextureSRV[0]);
			}

			srvdesc.Format = D3D11EffectCompiler::MakeSRGBFormat(texdesc.Format);

			if (SUCCEEDED(hr))
			{
				hr = this->mDevice->CreateShaderResourceView(this->mBackBufferTexture, &srvdesc, &this->mBackBufferTextureSRV[1]);
			}
		}

		if (FAILED(hr))
		{
			SAFE_RELEASE(this->mBackBufferReplacement);
			SAFE_RELEASE(this->mBackBufferTexture);
			SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
			SAFE_RELEASE(this->mBackBufferTextureSRV[1]);

			return false;
		}

		D3D11_RENDER_TARGET_VIEW_DESC rtdesc;
		ZeroMemory(&rtdesc, sizeof(D3D11_RENDER_TARGET_VIEW_DESC));
		rtdesc.Format = D3D11EffectCompiler::MakeNonSRBFormat(texdesc.Format);
		rtdesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		
		hr = this->mDevice->CreateRenderTargetView(this->mBackBuffer, &rtdesc, &this->mBackBufferTargets[0]);

		if (FAILED(hr))
		{
			SAFE_RELEASE(this->mBackBufferReplacement);
			SAFE_RELEASE(this->mBackBufferTexture);
			SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
			SAFE_RELEASE(this->mBackBufferTextureSRV[1]);

			return false;
		}

		rtdesc.Format = D3D11EffectCompiler::MakeSRGBFormat(texdesc.Format);
		
		hr = this->mDevice->CreateRenderTargetView(this->mBackBuffer, &rtdesc, &this->mBackBufferTargets[1]);

		if (FAILED(hr))
		{
			SAFE_RELEASE(this->mBackBufferReplacement);
			SAFE_RELEASE(this->mBackBufferTexture);
			SAFE_RELEASE(this->mBackBufferTextureSRV[0]);
			SAFE_RELEASE(this->mBackBufferTextureSRV[1]);
			SAFE_RELEASE(this->mBackBufferTargets[0]);

			return false;
		}

		return true;
	}
	bool D3D11Runtime::CreateDepthStencilReplacement(ID3D11DepthStencilView *depthstencil)
	{
		SAFE_RELEASE(this->mDepthStencil);
		SAFE_RELEASE(this->mDepthStencilReplacement);
		SAFE_RELEASE(this->mDepthStencilTexture);
		SAFE_RELEASE(this->mDepthStencilTextureSRV);

		if (depthstencil != nullptr)
		{
			this->mDepthStencil = depthstencil;
			this->mDepthStencil->AddRef();
			this->mDepthStencil->GetResource(reinterpret_cast<ID3D11Resource **>(&this->mDepthStencilTexture));
		
			D3D11_TEXTURE2D_DESC texdesc;
			this->mDepthStencilTexture->GetDesc(&texdesc);

			HRESULT hr = S_OK;

			if ((texdesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0)
			{
				SAFE_RELEASE(this->mDepthStencilTexture);

				switch (texdesc.Format)
				{
					case DXGI_FORMAT_R16_TYPELESS:
					case DXGI_FORMAT_D16_UNORM:
						texdesc.Format = DXGI_FORMAT_R16_TYPELESS;
						break;
					case DXGI_FORMAT_R32_TYPELESS:
					case DXGI_FORMAT_D32_FLOAT:
						texdesc.Format = DXGI_FORMAT_R32_TYPELESS;
						break;
					default:
					case DXGI_FORMAT_R24G8_TYPELESS:
					case DXGI_FORMAT_D24_UNORM_S8_UINT:
						texdesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
						break;
					case DXGI_FORMAT_R32G8X24_TYPELESS:
					case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
						texdesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
						break;
				}

				texdesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

				hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &this->mDepthStencilTexture);

				if (SUCCEEDED(hr))
				{
					D3D11_DEPTH_STENCIL_VIEW_DESC dsvdesc;
					ZeroMemory(&dsvdesc, sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC));
					dsvdesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

					switch (texdesc.Format)
					{
						case DXGI_FORMAT_R16_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D16_UNORM;
							break;
						case DXGI_FORMAT_R32_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D32_FLOAT;
							break;
						case DXGI_FORMAT_R24G8_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
							break;
						case DXGI_FORMAT_R32G8X24_TYPELESS:
							dsvdesc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
							break;
					}

					hr = this->mDevice->CreateDepthStencilView(this->mDepthStencilTexture, &dsvdesc, &this->mDepthStencilReplacement);
				}
			}

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create depthstencil replacement texture! HRESULT is '" << hr << "'.";

				SAFE_RELEASE(this->mDepthStencil);
				SAFE_RELEASE(this->mDepthStencilTexture);

				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc;
			ZeroMemory(&srvdesc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
			srvdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvdesc.Texture2D.MipLevels = 1;

			switch (texdesc.Format)
			{
				case DXGI_FORMAT_R16_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R16_FLOAT;
					break;
				case DXGI_FORMAT_R32_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R32_FLOAT;
					break;
				case DXGI_FORMAT_R24G8_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
					break;
				case DXGI_FORMAT_R32G8X24_TYPELESS:
					srvdesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
					break;
			}

			hr = this->mDevice->CreateShaderResourceView(this->mDepthStencilTexture, &srvdesc, &this->mDepthStencilTextureSRV);

			if (FAILED(hr))
			{
				LOG(TRACE) << "Failed to create depthstencil replacement resource view! HRESULT is '" << hr << "'.";

				SAFE_RELEASE(this->mDepthStencil);
				SAFE_RELEASE(this->mDepthStencilReplacement);
				SAFE_RELEASE(this->mDepthStencilTexture);

				return false;
			}

			if (this->mDepthStencil != this->mDepthStencilReplacement)
			{
				// Update auto depthstencil
				ID3D11RenderTargetView *targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = { nullptr };
				ID3D11DepthStencilView *depthstencil = nullptr;

				this->mImmediateContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, &depthstencil);

				if (depthstencil != nullptr)
				{
					if (depthstencil == this->mDepthStencil)
					{
						this->mImmediateContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, this->mDepthStencilReplacement);
					}

					depthstencil->Release();
				}

				for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
				{
					SAFE_RELEASE(targets[i]);
				}
			}
		}

		// Update effect textures
		D3D11Effect *effect = static_cast<D3D11Effect *>(this->mEffect.get());

		if (effect != nullptr)
		{
			for (auto &it : effect->mTextures)
			{
				D3D11Texture *texture = static_cast<D3D11Texture *>(it.second.get());

				if (texture->mSource == D3D11Texture::Source::DepthStencil)
				{
					texture->ChangeSource(this->mDepthStencilTextureSRV, nullptr);
				}
			}
		}

		return true;
	}

	std::unique_ptr<Effect> D3D11Runtime::CompileEffect(const EffectTree &ast, std::string &errors) const
	{
		std::unique_ptr<D3D11Effect> effect(new D3D11Effect(shared_from_this()));

		D3D11EffectCompiler visitor(ast);
		
		if (!visitor.Traverse(effect.get(), errors))
		{
			return nullptr;
		}

		D3D11_RASTERIZER_DESC rsdesc;
		ZeroMemory(&rsdesc, sizeof(D3D11_RASTERIZER_DESC));
		rsdesc.FillMode = D3D11_FILL_SOLID;
		rsdesc.CullMode = D3D11_CULL_NONE;
		rsdesc.DepthClipEnable = TRUE;

		HRESULT hr = this->mDevice->CreateRasterizerState(&rsdesc, &effect->mRasterizerState);

		if (FAILED(hr))
		{
			return nullptr;
		}

		return std::unique_ptr<Effect>(effect.release());
	}
	void D3D11Runtime::CreateScreenshot(unsigned char *buffer, std::size_t size) const
	{
		if (this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && this->mSwapChainDesc.BufferDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
		{
			LOG(WARNING) << "Screenshots are not supported for backbuffer format " << this->mSwapChainDesc.BufferDesc.Format << ".";
			return;
		}

		if (size < static_cast<std::size_t>(this->mSwapChainDesc.BufferDesc.Width * this->mSwapChainDesc.BufferDesc.Height * 4))
		{
			return;
		}

		D3D11_TEXTURE2D_DESC texdesc;
		ZeroMemory(&texdesc, sizeof(D3D11_TEXTURE2D_DESC));
		texdesc.Width = this->mSwapChainDesc.BufferDesc.Width;
		texdesc.Height = this->mSwapChainDesc.BufferDesc.Height;
		texdesc.ArraySize = 1;
		texdesc.MipLevels = 1;
		texdesc.Format = this->mSwapChainDesc.BufferDesc.Format;
		texdesc.SampleDesc.Count = 1;
		texdesc.SampleDesc.Quality = 0;
		texdesc.Usage = D3D11_USAGE_STAGING;
		texdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		ID3D11Texture2D *textureStaging = nullptr;
		HRESULT hr = this->mDevice->CreateTexture2D(&texdesc, nullptr, &textureStaging);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to create staging resource for screenshot capture! HRESULT is '" << hr << "'.";
			return;
		}

		this->mImmediateContext->CopyResource(textureStaging, this->mBackBuffer);

		D3D11_MAPPED_SUBRESOURCE mapped;
		hr = this->mImmediateContext->Map(textureStaging, 0, D3D11_MAP_READ, 0, &mapped);

		if (FAILED(hr))
		{
			LOG(TRACE) << "Failed to map staging resource with screenshot capture! HRESULT is '" << hr << "'.";

			textureStaging->Release();
			return;
		}

		BYTE *pMem = buffer;
		BYTE *pMapped = static_cast<BYTE *>(mapped.pData);

		const UINT pitch = texdesc.Width * 4;

		for (UINT y = 0; y < texdesc.Height; ++y)
		{
			CopyMemory(pMem, pMapped, std::min(pitch, static_cast<UINT>(mapped.RowPitch)));

			for (UINT x = 0; x < pitch; x += 4)
			{
				pMem[x + 3] = 0xFF;

				if (texdesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || texdesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
				{
					std::swap(pMem[x + 0], pMem[x + 2]);
				}
			}
								
			pMem += pitch;
			pMapped += mapped.RowPitch;
		}

		this->mImmediateContext->Unmap(textureStaging, 0);

		textureStaging->Release();
	}

	void D3D11Runtime::DumpFrameTrace(const boost::filesystem::path &path)
	{
		LOG(INFO) << "D3D11 - Beginning to dump frame into " << path.string().c_str();
		DumpRootPath = path;
		mIsDumpingTrace = true;
		swapBufferCount = 2;
	}

	void D3D11Runtime::ToggleDebugView(bool saveCurrent, bool playSave, bool playNext)
	{
		//LOG(INFO) << "D3D11 - Entering debug view.";
		//LOG(INFO) << "D3D11 - Debug RTs watched count " << WatchedRTs.size();

		if (playNext)
		{
			mToggleDebugViewID++;
			if (mToggleDebugViewID > WatchedRTs.size()) mToggleDebugViewID = 0;
			CurrentSavedView = -1;
		}

		if (saveCurrent) 
		{
			SavedViews.push_back(mToggleDebugViewID);
			CurrentSavedView = -1;
			LOG(INFO) << "SavedView " << mToggleDebugViewID;
		}

		if (playSave)
		{
			CurrentSavedView++;
			if (CurrentSavedView >= SavedViews.size())
			{
				CurrentSavedView = -1;
				mToggleDebugViewID = -1;
			}
			else 
			{
				mToggleDebugViewID = SavedViews.at(CurrentSavedView);
			}
		}

	}

	D3D11Effect::D3D11Effect(std::shared_ptr<const D3D11Runtime> runtime) : mRuntime(runtime), mRasterizerState(nullptr), mConstantsDirty(true)
	{
	}
	D3D11Effect::~D3D11Effect()
	{
		SAFE_RELEASE(this->mRasterizerState);

		for (auto &it : this->mSamplerStates)
		{
			it->Release();
		}
			
		for (auto &it : this->mConstantBuffers)
		{
			if (it != nullptr)
			{
				it->Release();
			}
		}
		for (auto &it : this->mConstantStorages)
		{
			free(it);
		}
	}

	void D3D11Effect::Begin() const
	{
		ID3D11DeviceContext *const devicecontext = this->mRuntime->mImmediateContext;

		// Setup vertex input
		const uintptr_t null = 0;
		devicecontext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		devicecontext->IASetInputLayout(nullptr);
		devicecontext->IASetVertexBuffers(0, 1, reinterpret_cast<ID3D11Buffer *const *>(&null), reinterpret_cast<const UINT *>(&null), reinterpret_cast<const UINT *>(&null));

		devicecontext->RSSetState(this->mRasterizerState);

		// Setup samplers
		devicecontext->VSSetSamplers(0, static_cast<UINT>(this->mSamplerStates.size()), this->mSamplerStates.data());
		devicecontext->PSSetSamplers(0, static_cast<UINT>(this->mSamplerStates.size()), this->mSamplerStates.data());

		// Setup shader constants
		devicecontext->VSSetConstantBuffers(0, static_cast<UINT>(this->mConstantBuffers.size()), this->mConstantBuffers.data());
		devicecontext->PSSetConstantBuffers(0, static_cast<UINT>(this->mConstantBuffers.size()), this->mConstantBuffers.data());

		// Setup depthstencil
		assert(this->mRuntime->mDefaultDepthStencil != nullptr);

		devicecontext->ClearDepthStencilView(this->mRuntime->mDefaultDepthStencil, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
	}
	void D3D11Effect::End() const
	{
	}

	D3D11Texture::D3D11Texture(D3D11Effect *effect, const Description &desc) : Texture(desc), mEffect(effect), mSource(Source::Memory), mTexture(nullptr), mShaderResourceView(), mRenderTargetView(), mRegister(0)
	{
	}
	D3D11Texture::~D3D11Texture()
	{
		SAFE_RELEASE(this->mRenderTargetView[0]);
		SAFE_RELEASE(this->mRenderTargetView[1]);
		SAFE_RELEASE(this->mShaderResourceView[0]);
		SAFE_RELEASE(this->mShaderResourceView[1]);

		SAFE_RELEASE(this->mTexture);
	}

	bool D3D11Texture::Update(unsigned int level, const unsigned char *data, std::size_t size)
	{
		if (data == nullptr || size == 0 || level > this->mDesc.Levels || this->mSource != Source::Memory)
		{
			return false;
		}

		assert(this->mDesc.Height != 0);

		ID3D11DeviceContext *context = this->mEffect->mRuntime->mImmediateContext;

		context->UpdateSubresource(this->mTexture, level, nullptr, data, static_cast<UINT>(size / this->mDesc.Height), static_cast<UINT>(size));

		if (level == 0 && this->mDesc.Levels > 1)
		{
			context->GenerateMips(this->mShaderResourceView[0]);
		}

		return true;
	}
	void D3D11Texture::ChangeSource(ID3D11ShaderResourceView *srv, ID3D11ShaderResourceView *srvSRGB)
	{
		if (srvSRGB == nullptr)
		{
			srvSRGB = srv;
		}

		if (srv == this->mShaderResourceView[0] && srvSRGB == this->mShaderResourceView[1])
		{
			return;
		}

		SAFE_RELEASE(this->mRenderTargetView[0]);
		SAFE_RELEASE(this->mRenderTargetView[1]);
		SAFE_RELEASE(this->mShaderResourceView[0]);
		SAFE_RELEASE(this->mShaderResourceView[1]);

		SAFE_RELEASE(this->mTexture);

		if (srv != nullptr)
		{
			this->mShaderResourceView[0] = srv;
			this->mShaderResourceView[0]->AddRef();
			this->mShaderResourceView[0]->GetResource(reinterpret_cast<ID3D11Resource **>(&this->mTexture));
			this->mShaderResourceView[1] = srvSRGB;
			this->mShaderResourceView[1]->AddRef();

			D3D11_TEXTURE2D_DESC texdesc;
			this->mTexture->GetDesc(&texdesc);

			this->mDesc.Width = texdesc.Width;
			this->mDesc.Height = texdesc.Height;
			this->mDesc.Format = Effect::Texture::Format::Unknown;
			this->mDesc.Levels = texdesc.MipLevels;
		}
		else
		{
			this->mDesc.Width = this->mDesc.Height = this->mDesc.Levels = 0;
			this->mDesc.Format = Effect::Texture::Format::Unknown;
		}

		// Update techniques shader resourceviews
		for (auto &technique : this->mEffect->mTechniques)
		{
			for (auto &pass : static_cast<D3D11Technique *>(technique.second.get())->mPasses)
			{
				pass.SRV[this->mRegister] = this->mShaderResourceView[0];
				pass.SRV[this->mRegister + 1] = this->mShaderResourceView[1];
			}
		}
	}

	D3D11Constant::D3D11Constant(D3D11Effect *effect, const Description &desc) : Constant(desc), mEffect(effect), mBufferIndex(0), mBufferOffset(0)
	{
	}
	D3D11Constant::~D3D11Constant()
	{
	}

	void D3D11Constant::GetValue(unsigned char *data, std::size_t size) const
	{
		size = std::min(size, this->mDesc.Size);

		CopyMemory(data, this->mEffect->mConstantStorages[this->mBufferIndex] + this->mBufferOffset, size);
	}
	void D3D11Constant::SetValue(const unsigned char *data, std::size_t size)
	{
		size = std::min(size, this->mDesc.Size);

		unsigned char *storage = this->mEffect->mConstantStorages[this->mBufferIndex] + this->mBufferOffset;

		if (std::memcmp(storage, data, size) == 0)
		{
			return;
		}

		CopyMemory(storage, data, size);

		this->mEffect->mConstantsDirty = true;
	}

	D3D11Technique::D3D11Technique(D3D11Effect *effect, const Description &desc) : Technique(desc), mEffect(effect)
	{
	}
	D3D11Technique::~D3D11Technique()
	{
		for (auto &pass : this->mPasses)
		{
			if (pass.VS != nullptr)
			{
				pass.VS->Release();
			}
			if (pass.PS != nullptr)
			{
				pass.PS->Release();
			}
			if (pass.BS != nullptr)
			{
				pass.BS->Release();
			}
			if (pass.DSS != nullptr)
			{
				pass.DSS->Release();
			}
		}
	}

	void D3D11Technique::RenderPass(unsigned int index) const
	{
		const std::shared_ptr<const D3D11Runtime> runtime = this->mEffect->mRuntime;
		ID3D11DeviceContext *const devicecontext = runtime->mImmediateContext;
		const D3D11Technique::Pass &pass = this->mPasses[index];

		// Update shader constants
		if (this->mEffect->mConstantsDirty)
		{
			for (std::size_t i = 0, count = this->mEffect->mConstantBuffers.size(); i < count; ++i)
			{
				ID3D11Buffer *buffer = this->mEffect->mConstantBuffers[i];
				const unsigned char *storage = this->mEffect->mConstantStorages[i];

				if (buffer == nullptr)
				{
					continue;
				}

				D3D11_MAPPED_SUBRESOURCE mapped;
				HRESULT hr = devicecontext->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);

				if (FAILED(hr))
				{
					LOG(TRACE) << "Failed to map constant buffer at slot " << i << "! HRESULT is '" << hr << "'!";

					continue;
				}

				CopyMemory(mapped.pData, storage, mapped.RowPitch);

				devicecontext->Unmap(buffer, 0);
			}

			this->mEffect->mConstantsDirty = false;
		}

		// Setup states
		devicecontext->VSSetShader(pass.VS, nullptr, 0);
		devicecontext->HSSetShader(nullptr, nullptr, 0);
		devicecontext->DSSetShader(nullptr, nullptr, 0);
		devicecontext->GSSetShader(nullptr, nullptr, 0);
		devicecontext->PSSetShader(pass.PS, nullptr, 0);

		const FLOAT blendfactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		devicecontext->OMSetBlendState(pass.BS, blendfactor, D3D11_DEFAULT_SAMPLE_MASK);
		devicecontext->OMSetDepthStencilState(pass.DSS, pass.StencilRef);

		// Save backbuffer of previous pass
		devicecontext->CopyResource(runtime->mBackBufferTexture, runtime->mBackBuffer);

		// Setup shader resources
		devicecontext->VSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), pass.SRV.data());
		devicecontext->PSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), pass.SRV.data());

		if (ReShade::Runtimes::D3D11Runtime::mToggleDebugViewID >= 0) 
		{
			int indexToWatch = ReShade::Runtimes::D3D11Runtime::mToggleDebugViewID;
			int index = 0;
			for (auto watchedRT : ReShade::Runtimes::D3D11Runtime::WatchedRTs) 
			{
				if (indexToWatch != index) 
				{
					index++;
					continue;
				}
				//Found our RT
				LOG(INFO) << "Overriding RT to backbuffer! Index " << index << " available: " << ReShade::Runtimes::D3D11Runtime::WatchedRTs.size();

				D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc;
				ZeroMemory(&srvdesc, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
				srvdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvdesc.Texture2D.MipLevels = 1;
				srvdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

				ID3D11Resource* resource;
				watchedRT->GetResource(&resource);

				ID3D11Texture2D *texture = nullptr;
				HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));

				D3D11_TEXTURE2D_DESC desc;
				texture->GetDesc(&desc);

				switch (desc.Format)
				{
					case DXGI_FORMAT_B8G8R8A8_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
						break;

					case DXGI_FORMAT_R16G16B16A16_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
						break;

					case DXGI_FORMAT_R8_TYPELESS:
						srvdesc.Format = DXGI_FORMAT_R8_UNORM;
						break;
				}

				if (FAILED(hr))
				{
					LOG(INFO) << "Was not a texture 2D!";
					break;
				}

				ID3D11ShaderResourceView* srv;
				hr = this->mEffect->mRuntime->mDevice->CreateShaderResourceView(texture, &srvdesc, &srv);

				if (FAILED(hr))
				{
					LOG(INFO) << "SRV creation failed! " << hr;
				}
				else 
				{
					devicecontext->PSSetShaderResources(0, 1, &srv);
				}

				break;

			}
		}

		// Setup rendertargets
		ID3D11DepthStencilView *depthstencil = runtime->mDefaultDepthStencil;

		if (pass.Viewport.Width != runtime->mSwapChainDesc.BufferDesc.Width || pass.Viewport.Height != runtime->mSwapChainDesc.BufferDesc.Height)
		{
			depthstencil = nullptr;
		}

		devicecontext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, pass.RT, depthstencil);
		devicecontext->RSSetViewports(1, &pass.Viewport);

		for (ID3D11RenderTargetView *rtv : pass.RT)
		{
			if (rtv == nullptr)
			{
				continue;
			}

			const FLOAT color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			devicecontext->ClearRenderTargetView(rtv, color);
		}
	
		// Draw triangle
		devicecontext->Draw(3, 0);

		// Reset shader resources
		ID3D11ShaderResourceView *null[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = { nullptr };
		devicecontext->VSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), null);
		devicecontext->PSSetShaderResources(0, static_cast<UINT>(pass.SRV.size()), null);

		// Reset rendertargets
		devicecontext->OMSetRenderTargets(0, nullptr, nullptr);

		// Update shader resources
		for (ID3D11ShaderResourceView *srv : pass.RTSRV)
		{
			if (srv == nullptr)
			{
				continue;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc;
			srv->GetDesc(&srvdesc);

			if (srvdesc.Texture2D.MipLevels > 1)
			{
				devicecontext->GenerateMips(srv);
			}
		}
	}
} }