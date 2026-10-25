#include "core/log.h"
#include "hle/hle.h"

namespace wf::hle {
namespace {

void ee_create(uint32_t workBuf, uint32_t region, uint32_t lang, uint32_t fsClient) { log_cat("hle", "ErrEulaCreate"); }
void ee_void() {}
void ee_void1(uint32_t) {}
uint32_t ee_zero() { return 0; }
bool ee_false() { return false; }
void ee_appear_error(uint32_t arg) { log_warn("game asked to show a system error dialog (erreula)"); }

WF_EXPORT_AS(erreula, ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient, ee_create);
WF_EXPORT_AS(erreula, ErrEulaDestroy__3RplFv, ee_void);
WF_EXPORT_AS(erreula, ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo, ee_void1);
WF_EXPORT_AS(erreula, ErrEulaDrawTV__3RplFv, ee_void);
WF_EXPORT_AS(erreula, ErrEulaDrawDRC__3RplFv, ee_void);
WF_EXPORT_AS(erreula, ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg, ee_appear_error);
WF_EXPORT_AS(erreula, ErrEulaDisappearError__3RplFv, ee_void);
WF_EXPORT_AS(erreula, ErrEulaGetStateErrorViewer__3RplFv, ee_zero);
WF_EXPORT_AS(erreula, ErrEulaIsDecideSelectButtonError__3RplFv, ee_false);
WF_EXPORT_AS(erreula, ErrEulaIsDecideSelectLeftButtonError__3RplFv, ee_false);
WF_EXPORT_AS(erreula, ErrEulaIsDecideSelectRightButtonError__3RplFv, ee_false);
WF_EXPORT_AS(erreula, ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType, ee_void1);
WF_EXPORT_AS(erreula, ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg, ee_void1);
WF_EXPORT_AS(erreula, ErrEulaIsAppearHomeNixSign__3RplFv, ee_false);
WF_EXPORT_AS(erreula, ErrEulaDisappearHomeNixSign__3RplFv, ee_void);
WF_EXPORT_AS(erreula, ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType, ee_void1);
WF_EXPORT_AS(erreula, ErrEulaIsSelectCursorActive__3RplFv, ee_false);
WF_EXPORT_AS(erreula, ErrEulaGetResultType__3RplFv, ee_zero);
WF_EXPORT_AS(erreula, ErrEulaGetResultCode__3RplFv, ee_zero);
WF_EXPORT_AS(erreula, ErrEulaGetSelectButtonNumError__3RplFv, ee_zero);

}
}
