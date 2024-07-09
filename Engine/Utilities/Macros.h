#pragma once

#define ARG_COUNT_IMPL(												\
_0, _1, _2, _3, _4, _5, _6, _7, _8, _9,								\
_10, _11, _12, _13, _14, _15, _16, _17, _18, _19,					\
_20, _21, _22, _23, _24, _25, _26, _27, _28, _29,					\
_30, _31, _32, _33, _34, _35, _36, _37, _38, _39,					\
_40, _41, _42, _43, _44, _45, _46, _47, _48, _49,					\
_50, _51, _52, _53, _54, _55, _56, _57, _58, _59,					\
_60, _61, _62, _63, _64, N, ...) N									

#define ARG_COUNT(...) ARG_COUNT_IMPL(0, ##__VA_ARGS__,				\
	64, 63, 62, 61, 60,												\
	59, 58, 57, 56, 55, 54, 53, 52, 51, 50,							\
	49, 48, 47, 46, 45, 44, 43, 42, 41, 40,							\
	39, 38, 37, 36, 35, 34, 33, 32, 31, 30,							\
	29, 28, 27, 26, 25, 24, 23, 22, 21, 20,							\
	19, 18, 17, 16, 15, 14, 13, 12, 11, 10,							\
	 9,  8,  7,  6,  5,  4,  3,  2,  1,  0)

#define REFLECT_EXPAND_2(x) x
#define REFLECT_EXPAND(x) REFLECT_EXPAND_2(x)
#define REFLECT_CONCATE_2(x, y) x##y
#define REFLECT_CONCATE(x, y) REFLECT_CONCATE_2(x, y)

#define CONCATE_STRUCT_NAME_1(name) name
#define CONCATE_STRUCT_NAME_2(name1, name2) name1##name2
#define CONCATE_STRUCT_NAME_3(name1, name2, name3) name1##name2##name3
#define CONCATE_STRUCT_NAME_4(name1, name2, name3, name4) name1##name2##name3##name4
#define CONCATE_STRUCT_NAME_IMPL(...) REFLECT_EXPAND(REFLECT_CONCATE(CONCATE_STRUCT_NAME_, ARG_COUNT(__VA_ARGS__))(__VA_ARGS__))
#define CONCATE_STRUCT_NAME(...) CONCATE_STRUCT_NAME_IMPL(__VA_ARGS__)

#define CONCATE_STRUCT_NAME_2_WITH_UNDERLINE(name1, name2) name1##_##name2
#define CONCATE_STRUCT_NAME_3_WITH_UNDERLINE(name1, name2, name3) name1##_##name2##_##name3
#define CONCATE_STRUCT_NAME_4_WITH_UNDERLINE(name1, name2, name3, name4) name1##_##name2##_##name3##_##name4
#define CONCATE_STRUCT_NAME_WITH_UNDERLINE_IMPL(...) REFLECT_EXPAND(REFLECT_CONCATE(REFLECT_CONCATE(CONCATE_STRUCT_NAME_, ARG_COUNT(__VA_ARGS__)), _WITH_UNDERLINE)(__VA_ARGS__))
#define CONCATE_STRUCT_NAME_WITH_UNDERLINE(...) CONCATE_STRUCT_NAME_WITH_UNDERLINE_IMPL(__VA_ARGS__)
