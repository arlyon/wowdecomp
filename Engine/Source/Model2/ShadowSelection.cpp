#include "ShadowSelection.h"
#include "M2Model.h"
#include "M2Shared.h"
namespace M2Shadow {
float BatchOpacity(const CM2Model& model,const M2Batch& batch) {
    const auto& data=*model.m_shared->m_data;
    float alpha=model.m_alpha;
    if(batch.m_colorIndex<data.m_colors.m_count)alpha*=model.m_colors[batch.m_colorIndex].m_alphaTrack.m_currentValue;
    if(batch.m_textureCount)alpha*=model.m_textureWeights[data.m_textureWeightCombos[batch.m_textureWeightComboIndex]].m_currentValue;
    return alpha;
}
}
