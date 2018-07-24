#pragma once

#include "EffectParserTree.hpp"
#include <algorithm> 

namespace ReShade
{
	EffectTree::Index OptimizeExpression(EffectTree &ast, EffectTree::Index expression);
}