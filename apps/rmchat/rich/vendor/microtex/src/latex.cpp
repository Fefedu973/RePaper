#include "latex.h"

#include "core/core.h"
#include "core/formula.h"
#include "core/macro.h"
#include "fonts/fonts.h"
#if CLATEX_CXX17
#include <filesystem>
#endif

using namespace std;
using namespace tex;

string tex::RES_BASE = "res";
static string CHECK_FILE = ".clatexmath-res_root";
#ifdef _WIN32
static char PATH_SEPERATOR = ';';
#else
static char PATH_SEPERATOR = ':';
#endif

Formula* LaTeX::_formula = nullptr;
TeXRenderBuilder* LaTeX::_builder = nullptr;

string LaTeX::queryResourceLocation(string& custom_path) {
  // RMChat: resource discovery is disabled; only explicit Qt resources are used.
  return custom_path;
}

void LaTeX::init(string res_root_path) {
  // RMChat: only the application's embedded, explicitly supplied resources.
  RES_BASE = res_root_path;
  if (_formula != nullptr) return;

  NewCommandMacro::_init_();
  DefaultTeXFont::_init_();
  Formula::_init_();
  TextRenderingBox::_init_();

  _formula = new Formula();
  _builder = new TeXRenderBuilder();
}

void LaTeX::release() {
  DefaultTeXFont::_free_();
  Formula::_free_();
  MacroInfo::_free_();
  NewCommandMacro::_free_();
  TextRenderingBox::_free_();

  if (_formula != nullptr) delete _formula;
  if (_builder != nullptr) delete _builder;
}

const string& LaTeX::getResRootPath() {
  return RES_BASE;
}

void LaTeX::setDebug(bool debug) {
  Formula::setDEBUG(debug);
}

TeXRender* LaTeX::parse(const wstring& latex, int width, float textSize, float lineSpace, color fg) {
  bool lined = true;
  if (startswith(latex, L"$$") || startswith(latex, L"\\[")) {
    lined = false;
  }
  Alignment align = lined ? Alignment::left : Alignment::center;
  _formula->setLaTeX(latex);
  TeXRender* render =
    _builder->setStyle(TexStyle::display)
      .setTextSize(textSize)
      .setWidth(UnitType::pixel, width, align)
      .setIsMaxWidth(lined)
      .setLineSpace(UnitType::pixel, lineSpace)
      .setForeground(fg)
      .build(*_formula);
  return render;
}
