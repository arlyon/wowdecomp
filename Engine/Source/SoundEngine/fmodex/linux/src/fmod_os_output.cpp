#include "fmod_output_sdl.h"
#include "fmod_pluginfactory.h"
FMOD_RESULT FMOD_OS_Output_Register(FMOD::PluginFactory *factory)
{
    if (!factory) return FMOD_ERR_INVALID_PARAM;
    return factory->registerOutput(FMOD::getSDLDescription());
}
FMOD_RESULT FMOD_OS_Output_GetDefault(FMOD_OUTPUTTYPE *type)
{
    if (!type) return FMOD_ERR_INVALID_PARAM;
    *type = FMOD_OUTPUTTYPE_UNKNOWN; // Public FMOD enum for third-party outputs.
    return FMOD_OK;
}
