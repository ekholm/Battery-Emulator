// Undefines every BE_ROW_* macro after a consumer block, so the next
// consumer starts clean. Deliberately no include guard.
#undef BE_ROW_UintSetting
#undef BE_ROW_IntSetting
#undef BE_ROW_BoolSetting
#undef BE_ROW_StringSetting
#undef BE_ROW_ScaledSetting
#undef BE_ROW_InstantUintSetting
#undef BE_ROW_InstantIntSetting
#undef BE_ROW_InstantScaledSetting
#undef BE_ROW_InstantBoolSetting
#undef BE_ROW_VolatileUintSetting
#undef BE_ROW_VolatileBoolSetting
#undef BE_ROW_VolatileFloatSetting
#undef BE_ROW_VolatileScaledSetting
