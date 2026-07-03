/**
 * seq_ledger_export.h — libseq_ledger.so 导出宏
 *
 * 功能：编译 seq_ledger 时导出公共 API；使用方 include 时导入声明。
 */
#ifndef SEQ_LEDGER_EXPORT_H
#define SEQ_LEDGER_EXPORT_H

#if defined(_WIN32)
#  if defined(SEQ_LEDGER_BUILD_SHARED)
#    define SEQ_LEDGER_API __declspec(dllexport)
#  else
#    define SEQ_LEDGER_API __declspec(dllimport)
#  endif
#else
#  if defined(SEQ_LEDGER_BUILD_SHARED)
#    define SEQ_LEDGER_API __attribute__((visibility("default")))
#  else
#    define SEQ_LEDGER_API
#  endif
#endif

#endif // SEQ_LEDGER_EXPORT_H
