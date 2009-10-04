/* Residual - A 3D game interpreter
 *
 * Residual is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the AUTHORS
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * $URL$
 * $Id$
 *
 */

#include "common/util.h"
#include "common/system.h"
#include "common/events.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "common/unzip.h"

#include "graphics/surface.h"
#include "graphics/colormasks.h"
#include "graphics/imagedec.h"
#include "graphics/cursorman.h"
#include "graphics/VectorRenderer.h"

#include "gui/launcher.h"
#include "gui/ThemeEngine.h"
#include "gui/ThemeEval.h"
#include "gui/ThemeParser.h"

#if defined(MACOSX) || defined(IPHONE)
#include <CoreFoundation/CoreFoundation.h>
#endif

#define GUI_ENABLE_BUILTIN_THEME

namespace GUI {

const char * const ThemeEngine::kImageLogo = "logo.bmp";
const char * const ThemeEngine::kImageLogoSmall = "logo_small.bmp";
const char * const ThemeEngine::kImageSearch = "search.bmp";

struct TextDrawData {
	const Graphics::Font *_fontPtr;

	struct {
		uint8 r, g, b;
	} _color;
};

struct WidgetDrawData {
	/** List of all the steps needed to draw this widget */
	Common::List<Graphics::DrawStep> _steps;

	TextData _textDataId;
	Graphics::TextAlign _textAlignH;
	GUI::ThemeEngine::TextAlignVertical _textAlignV;

	/** Extra space that the widget occupies when it's drawn.
	    E.g. when taking into account rounded corners, drop shadows, etc
		Used when restoring the widget background */
	uint16 _backgroundOffset;

	bool _buffer;


	/**
	 *	Calculates the background threshold offset of a given DrawData item.
	 *	After fully loading all DrawSteps of a DrawData item, this function must be
	 *	called in order to calculate if such draw steps would be drawn outside of
	 *	the actual widget drawing zone (e.g. shadows). If this is the case, a constant
	 *	value will be added when restoring the background of the widget.
	 */
	void calcBackgroundOffset();
};

class ThemeItem {

public:
	ThemeItem(ThemeEngine *engine, const Common::Rect &area) :
		_engine(engine), _area(area) {}
	virtual ~ThemeItem() {}

	virtual void drawSelf(bool doDraw, bool doRestore) = 0;

protected:
	ThemeEngine *_engine;
	Common::Rect _area;
};

class ThemeItemDrawData : public ThemeItem {
public:
	ThemeItemDrawData(ThemeEngine *engine, const WidgetDrawData *data, const Common::Rect &area, uint32 dynData) :
		ThemeItem(engine, area), _dynamicData(dynData), _data(data) {}

	void drawSelf(bool draw, bool restore);

protected:
	uint32 _dynamicData;
	const WidgetDrawData *_data;
};

class ThemeItemTextData : public ThemeItem {
public:
	ThemeItemTextData(ThemeEngine *engine, const TextDrawData *data, const Common::Rect &area, const Common::String &text,
		Graphics::TextAlign alignH, GUI::ThemeEngine::TextAlignVertical alignV,
		bool ellipsis, bool restoreBg, int deltaX) :
		ThemeItem(engine, area), _data(data), _text(text), _alignH(alignH), _alignV(alignV),
		_ellipsis(ellipsis), _restoreBg(restoreBg), _deltax(deltaX) {}

	void drawSelf(bool draw, bool restore);

protected:
	const TextDrawData *_data;
	Common::String _text;
	Graphics::TextAlign _alignH;
	GUI::ThemeEngine::TextAlignVertical _alignV;
	bool _ellipsis;
	bool _restoreBg;
	int _deltax;
};

class ThemeItemBitmap : public ThemeItem {
public:
	ThemeItemBitmap(ThemeEngine *engine, const Common::Rect &area, const Graphics::Surface *bitmap, bool alpha) :
		ThemeItem(engine, area), _bitmap(bitmap), _alpha(alpha) {}

	void drawSelf(bool draw, bool restore);

protected:
	const Graphics::Surface *_bitmap;
	bool _alpha;
};



/**********************************************************
 *	Data definitions for theme engine elements
 *********************************************************/
struct DrawDataInfo {
	DrawData id;		//!< The actual ID of the DrawData item.
	const char *name;	//!< The name of the DrawData item as it appears in the Theme Description files
	bool buffer;		//!< Sets whether this item is buffered on the backbuffer or drawn directly to the screen.
	DrawData parent;	//!< Parent DrawData item, for items that overlay. E.g. kButtonIdle -> kButtonHover
};

/**
 * Default values for each DrawData item.
 */
static const DrawDataInfo kDrawDataDefaults[] = {
	{kDDMainDialogBackground,		"mainmenu_bg",		true,	kDDNone},
	{kDDSpecialColorBackground,		"special_bg",		true,	kDDNone},
	{kDDPlainColorBackground,		"plain_bg",			true,	kDDNone},
	{kDDDefaultBackground,			"default_bg",		true,	kDDNone},
	{kDDTextSelectionBackground,	"text_selection",	false,	kDDNone},

	{kDDWidgetBackgroundDefault,	"widget_default",	true,	kDDNone},
	{kDDWidgetBackgroundSmall,		"widget_small",		true,	kDDNone},
	{kDDWidgetBackgroundEditText,	"widget_textedit",	true,	kDDNone},
	{kDDWidgetBackgroundSlider,		"widget_slider",	true,	kDDNone},

	{kDDButtonIdle,					"button_idle",		true,	kDDWidgetBackgroundSlider},
	{kDDButtonHover,				"button_hover",	false,	kDDButtonIdle},
	{kDDButtonDisabled,				"button_disabled",	true,	kDDNone},

	{kDDSliderFull,					"slider_full",		false,	kDDNone},
	{kDDSliderHover,				"slider_hover",		false,	kDDNone},
	{kDDSliderDisabled,				"slider_disabled",	true,	kDDNone},

	{kDDCheckboxDefault,			"checkbox_default",			true,	kDDNone},
	{kDDCheckboxDisabled,			"checkbox_disabled",		true,	kDDNone},
	{kDDCheckboxSelected,			"checkbox_selected",		false,	kDDCheckboxDefault},

	{kDDTabActive,					"tab_active",				false,	kDDTabInactive},
	{kDDTabInactive,				"tab_inactive",				true,	kDDNone},
	{kDDTabBackground,				"tab_background",			true,	kDDNone},

	{kDDScrollbarBase,				"scrollbar_base",			true,	kDDNone},

	{kDDScrollbarButtonIdle,		"scrollbar_button_idle",	true,	kDDNone},
	{kDDScrollbarButtonHover,		"scrollbar_button_hover",	false,	kDDScrollbarButtonIdle},

	{kDDScrollbarHandleIdle,		"scrollbar_handle_idle",	false,	kDDNone},
	{kDDScrollbarHandleHover,		"scrollbar_handle_hover",	false,	kDDScrollbarBase},

	{kDDPopUpIdle,					"popup_idle",	true,	kDDNone},
	{kDDPopUpHover,					"popup_hover",	false,	kDDPopUpIdle},
	{kDDPopUpDisabled,				"popup_disabled",	true,	kDDNone},

	{kDDCaret,						"caret",		false,	kDDNone},
	{kDDSeparator,					"separator",	true,	kDDNone},
};


/**********************************************************
 *	ThemeItem functions for drawing queues.
 *********************************************************/
void ThemeItemDrawData::drawSelf(bool draw, bool restore) {

	Common::Rect extendedRect = _area;
	extendedRect.grow(_engine->kDirtyRectangleThreshold + _data->_backgroundOffset);

	if (restore)
		_engine->restoreBackground(extendedRect);

	if (draw) {
		Common::List<Graphics::DrawStep>::const_iterator step;
		for (step = _data->_steps.begin(); step != _data->_steps.end(); ++step)
			_engine->renderer()->drawStep(_area, *step, _dynamicData);
	}

	_engine->addDirtyRect(extendedRect);
}

void ThemeItemTextData::drawSelf(bool draw, bool restore) {
	if (_restoreBg || restore)
		_engine->restoreBackground(_area);

	if (draw) {
		_engine->renderer()->setFgColor(_data->_color.r, _data->_color.g, _data->_color.b);
		_engine->renderer()->drawString(_data->_fontPtr, _text, _area, _alignH, _alignV, _deltax, _ellipsis);
	}

	_engine->addDirtyRect(_area);
}

void ThemeItemBitmap::drawSelf(bool draw, bool restore) {
	if (restore)
		_engine->restoreBackground(_area);

	if (draw) {
		if (_alpha)
			_engine->renderer()->blitAlphaBitmap(_bitmap, _area);
		else
			_engine->renderer()->blitSubSurface(_bitmap, _area);
	}

	_engine->addDirtyRect(_area);
}



/**********************************************************
 *	ThemeEngine class
 *********************************************************/
ThemeEngine::ThemeEngine(Common::String id, GraphicsMode mode) :
	_system(0), _vectorRenderer(0),
	_buffering(false), _bytesPerPixel(0),  _graphicsMode(kGfxDisabled),
	_font(0), _initOk(false), _themeOk(false), _enabled(false), _cursor(0) {

	_system = g_system;
	_parser = new ThemeParser(this);
	_themeEval = new GUI::ThemeEval();

	_useCursor = false;

	for (int i = 0; i < kDrawDataMAX; ++i) {
		_widgets[i] = 0;
	}

	for (int i = 0; i < kTextDataMAX; ++i) {
		_texts[i] = 0;
	}

	// We currently allow two different ways of theme selection in our config file:
	// 1) Via full path
	// 2) Via a basename, which will need to be translated into a full path
	// This function assures we have a correct path to pass to the ThemeEngine
	// constructor.
	_themeFile = getThemeFile(id);
	// We will use getThemeId to retrive the theme id from the given filename
	// here, since the user could have passed a fixed filename as 'id'.
	_themeId = getThemeId(_themeFile);

	_graphicsMode = mode;
	_themeArchive = 0;
	_initOk = false;
}

ThemeEngine::~ThemeEngine() {
	delete _vectorRenderer;
	_vectorRenderer = 0;
	_screen.free();
	_backBuffer.free();

	unloadTheme();

	// Release all graphics surfaces
	for (ImagesMap::iterator i = _bitmaps.begin(); i != _bitmaps.end(); ++i) {
		Graphics::Surface *surf = i->_value;
		if (surf) {
			surf->free();
			delete surf;
		}
	}
	_bitmaps.clear();

	delete _parser;
	delete _themeEval;
	delete[] _cursor;
	delete _themeArchive;
}



/**********************************************************
 *	Rendering mode management
 *********************************************************/
const ThemeEngine::Renderer ThemeEngine::_rendererModes[] = {
	{ "Disabled GFX", "none", kGfxDisabled },
	{ "Standard Renderer (16bpp)", "normal_16bpp", kGfxStandard16bit },
#ifndef DISABLE_FANCY_THEMES
	{ "Antialiased Renderer (16bpp)", "aa_16bpp", kGfxAntialias16bit }
#endif
};

const uint ThemeEngine::_rendererModesSize = ARRAYSIZE(ThemeEngine::_rendererModes);

const ThemeEngine::GraphicsMode ThemeEngine::_defaultRendererMode =
#ifndef DISABLE_FANCY_THEMES
	ThemeEngine::kGfxAntialias16bit;
#else
	ThemeEngine::kGfxStandard16bit;
#endif

ThemeEngine::GraphicsMode ThemeEngine::findMode(const Common::String &cfg) {
	for (uint i = 0; i < _rendererModesSize; ++i) {
		if (cfg.equalsIgnoreCase(_rendererModes[i].cfg))
			return _rendererModes[i].mode;
	}

	return kGfxDisabled;
}

const char *ThemeEngine::findModeConfigName(GraphicsMode mode) {
	for (uint i = 0; i < _rendererModesSize; ++i) {
		if (mode == _rendererModes[i].mode)
			return _rendererModes[i].cfg;
	}

	return findModeConfigName(kGfxDisabled);
}





/**********************************************************
 *	Theme setup/initialization
 *********************************************************/
bool ThemeEngine::init() {
	// reset everything and reload the graphics
	_initOk = false;
	setGraphicsMode(_graphicsMode);
	_overlayFormat = _system->getOverlayFormat();

	if (_screen.pixels && _backBuffer.pixels) {
		_initOk = true;
	}

	// TODO: Instead of hard coding the font here, it should be possible
	// to specify the fonts to be used for each resolution in the theme XML.
	if (_screen.w >= 400 && _screen.h >= 300) {
		_font = FontMan.getFontByUsage(Graphics::FontManager::kBigGUIFont);
	} else {
		_font = FontMan.getFontByUsage(Graphics::FontManager::kGUIFont);
	}

	// Try to create a Common::Archive with the files of the theme.
	if (!_themeArchive && !_themeFile.empty()) {
		Common::FSNode node(_themeFile);
		if (node.getName().hasSuffix(".zip") && !node.isDirectory()) {
#ifdef USE_ZLIB
			Common::ZipArchive *zipArchive = new Common::ZipArchive(node);

			if (!zipArchive || !zipArchive->isOpen()) {
				delete zipArchive;
				zipArchive = 0;
				warning("Failed to open Zip archive '%s'.", node.getPath().c_str());
			}
			_themeArchive = zipArchive;
#else
			warning("Trying to load theme '%s' in a Zip archive without zLib support", _themeFile.c_str());
			return false;
#endif
		} else if (node.isDirectory()) {
			_themeArchive = new Common::FSDirectory(node);
		}
	}

	// Load the theme
	// We pass the theme file here by default, so the user will
	// have a descriptive error message. The only exception will
	// be the builtin theme which has no filename.
	loadTheme(_themeFile.empty() ? _themeId : _themeFile);

	return ready();
}

void ThemeEngine::clearAll() {
	if (_initOk) {
		_system->clearOverlay();
		_system->grabOverlay((OverlayColor *)_screen.pixels, _screen.w);
	}
}

void ThemeEngine::refresh() {

	// Flush all bitmaps if the overlay pixel format changed.
	if (_overlayFormat != _system->getOverlayFormat()) {
		for (ImagesMap::iterator i = _bitmaps.begin(); i != _bitmaps.end(); ++i) {
			Graphics::Surface *surf = i->_value;
			if (surf) {
				surf->free();
				delete surf;
			}
		}
		_bitmaps.clear();
	}

	init();

	if (_enabled) {
		_system->showOverlay();

		if (_useCursor) {
			CursorMan.replaceCursorPalette(_cursorPal, 0, _cursorPalSize);
			CursorMan.replaceCursor(_cursor, _cursorWidth, _cursorHeight, _cursorHotspotX, _cursorHotspotY, 255, _cursorTargetScale);
		}
	}
}

void ThemeEngine::enable() {
	if (_enabled)
		return;

	if (_useCursor) {
		CursorMan.pushCursorPalette(_cursorPal, 0, _cursorPalSize);
		CursorMan.pushCursor(_cursor, _cursorWidth, _cursorHeight, _cursorHotspotX, _cursorHotspotY, 255, _cursorTargetScale);
		CursorMan.showMouse(true);
	}

	_system->showOverlay();
	clearAll();
	_enabled = true;
}

void ThemeEngine::disable() {
	if (!_enabled)
		return;

	_system->hideOverlay();

	if (_useCursor) {
		CursorMan.popCursorPalette();
		CursorMan.popCursor();
	}

	_enabled = false;
}

void ThemeEngine::setGraphicsMode(GraphicsMode mode) {
	switch (mode) {
	case kGfxStandard16bit:
#ifndef DISABLE_FANCY_THEMES
	case kGfxAntialias16bit:
#endif
		_bytesPerPixel = sizeof(uint16);
		break;

	default:
		error("Invalid graphics mode");
	}

	uint32 width = _system->getOverlayWidth();
	uint32 height = _system->getOverlayHeight();

	_backBuffer.free();
	_backBuffer.create(width, height, _bytesPerPixel);

	_screen.free();
	_screen.create(width, height, _bytesPerPixel);

	delete _vectorRenderer;
	_vectorRenderer = Graphics::createRenderer(mode);
	_vectorRenderer->setSurface(&_screen);
}

void WidgetDrawData::calcBackgroundOffset() {
	uint maxShadow = 0;
	for (Common::List<Graphics::DrawStep>::const_iterator step = _steps.begin();
		step != _steps.end(); ++step) {
		if ((step->autoWidth || step->autoHeight) && step->shadow > maxShadow)
			maxShadow = step->shadow;

		if (step->drawingCall == &Graphics::VectorRenderer::drawCallback_BEVELSQ && step->bevel > maxShadow)
			maxShadow = step->bevel;
	}

	_backgroundOffset = maxShadow;
}

void ThemeEngine::restoreBackground(Common::Rect r) {
	r.clip(_screen.w, _screen.h);
	_vectorRenderer->blitSurface(&_backBuffer, r);
}



/**********************************************************
 *	Theme elements management
 *********************************************************/
void ThemeEngine::addDrawStep(const Common::String &drawDataId, const Graphics::DrawStep &step) {
	DrawData id = parseDrawDataId(drawDataId);

	assert(_widgets[id] != 0);
	_widgets[id]->_steps.push_back(step);
}

bool ThemeEngine::addTextData(const Common::String &drawDataId, TextData textId, Graphics::TextAlign alignH, TextAlignVertical alignV) {
	DrawData id = parseDrawDataId(drawDataId);

	if (id == -1 || textId == -1 || !_widgets[id])
		return false;

	_widgets[id]->_textDataId = textId;
	_widgets[id]->_textAlignH = alignH;
	_widgets[id]->_textAlignV = alignV;

	return true;
}

bool ThemeEngine::addFont(TextData textId, const Common::String &file, int r, int g, int b) {
	if (textId == -1)
		return false;

	if (_texts[textId] != 0)
		delete _texts[textId];

	_texts[textId] = new TextDrawData;

	if (file == "default") {
		_texts[textId]->_fontPtr = _font;
	} else {
		_texts[textId]->_fontPtr = FontMan.getFontByName(file);

		if (!_texts[textId]->_fontPtr) {
			_texts[textId]->_fontPtr = loadFont(file);

			if (!_texts[textId]->_fontPtr)
				error("Couldn't load font '%s'", file.c_str());

			FontMan.assignFontToName(file, _texts[textId]->_fontPtr);
		}
	}

	_texts[textId]->_color.r = r;
	_texts[textId]->_color.g = g;
	_texts[textId]->_color.b = b;
	return true;

}

bool ThemeEngine::addBitmap(const Common::String &filename) {
	// Nothing has to be done if the bitmap already has been loaded.
	Graphics::Surface *surf = _bitmaps[filename];
	if (surf)
		return true;

	// If not, try to load the bitmap via the ImageDecoder class.
	surf = Graphics::ImageDecoder::loadFile(filename, _overlayFormat);
	if (!surf && _themeArchive) {
		Common::SeekableReadStream *stream = _themeArchive->createReadStreamForMember(filename);
		if (stream) {
			surf = Graphics::ImageDecoder::loadFile(*stream, _overlayFormat);
			delete stream;
		}
	}

	// Store the surface into our hashmap (attention, may store NULL entries!)
	_bitmaps[filename] = surf;

	return surf != 0;
}

bool ThemeEngine::addDrawData(const Common::String &data, bool cached) {
	DrawData id = parseDrawDataId(data);

	if (id == -1)
		return false;

	if (_widgets[id] != 0)
		delete _widgets[id];

	_widgets[id] = new WidgetDrawData;
	_widgets[id]->_buffer = kDrawDataDefaults[id].buffer;
	_widgets[id]->_textDataId = kTextDataNone;

	return true;
}


/**********************************************************
 *	Theme XML loading
 *********************************************************/
void ThemeEngine::loadTheme(const Common::String &themeId) {
	unloadTheme();

	if (themeId == "builtin") {
		_themeOk = loadDefaultXML();
	} else {
		// Load the archive containing image and XML data
		_themeOk = loadThemeXML(themeId);
	}

	if (!_themeOk) {
		warning("Failed to load theme '%s'", themeId.c_str());
		return;
	}

	for (int i = 0; i < kDrawDataMAX; ++i) {
		if (_widgets[i] == 0) {
			warning("Missing data asset: '%s'", kDrawDataDefaults[i].name);
		} else {
			_widgets[i]->calcBackgroundOffset();
		}
	}
}

void ThemeEngine::unloadTheme() {
	if (!_themeOk)
		return;

	for (int i = 0; i < kDrawDataMAX; ++i) {
		delete _widgets[i];
		_widgets[i] = 0;
	}

	for (int i = 0; i < kTextDataMAX; ++i) {
		delete _texts[i];
		_texts[i] = 0;
	}

	_themeEval->reset();
	_themeOk = false;
}

bool ThemeEngine::loadDefaultXML() {

	// The default XML theme is included on runtime from a pregenerated
	// file inside the themes directory.
	// Use the Python script "makedeftheme.py" to convert a normal XML theme
	// into the "default.inc" file, which is ready to be included in the code.
#ifdef GUI_ENABLE_BUILTIN_THEME
	const char *defaultXML =
#include "themes/default.inc"
	;

	if (!_parser->loadBuffer((const byte*)defaultXML, strlen(defaultXML), false))
		return false;

	_themeName = "ScummVM Classic Theme (Builtin Version)";
	_themeId = "builtin";
	_themeFile.clear();

	bool result = _parser->parse();
	_parser->close();

	return result;
#else
	warning("The built-in theme is not enabled in the current build. Please load an external theme");
	return false;
#endif
}

bool ThemeEngine::loadThemeXML(const Common::String &themeId) {
	assert(_parser);
	assert(_themeArchive);

	_themeName.clear();


	//
	// Now that we have a Common::Archive, verify that it contains a valid THEMERC File
	//
	Common::File themercFile;
	themercFile.open("THEMERC", *_themeArchive);
	if (!themercFile.isOpen()) {
		warning("Theme '%s' contains no 'THEMERC' file.", themeId.c_str());
		return false;
	}

	Common::String stxHeader = themercFile.readLine();
	if (!themeConfigParseHeader(stxHeader, _themeName) || _themeName.empty()) {
		warning("Corrupted 'THEMERC' file in theme '%s'", themeId.c_str());
		return false;
	}

	Common::ArchiveMemberList members;
	if (0 == _themeArchive->listMatchingMembers(members, "*.stx")) {
		warning("Found no STX files for theme '%s'.", themeId.c_str());
		return false;
	}

	//
	// Loop over all STX files, load and parse them
	//
	for (Common::ArchiveMemberList::iterator i = members.begin(); i != members.end(); ++i) {
		assert((*i)->getName().hasSuffix(".stx"));

		if (_parser->loadStream((*i)->createReadStream()) == false) {
			warning("Failed to load STX file '%s'", (*i)->getDisplayName().c_str());
			_parser->close();
			return false;
		}

		if (_parser->parse() == false) {
			warning("Failed to parse STX file '%s'", (*i)->getDisplayName().c_str());
			_parser->close();
			return false;
		}

		_parser->close();
	}

	assert(!_themeName.empty());
	return true;
}



/**********************************************************
 *	Drawing Queue management
 *********************************************************/
void ThemeEngine::queueDD(DrawData type, const Common::Rect &r, uint32 dynamic, bool restore) {
	if (_widgets[type] == 0)
		return;

	Common::Rect area = r;
	area.clip(_screen.w, _screen.h);

	ThemeItemDrawData *q = new ThemeItemDrawData(this, _widgets[type], area, dynamic);

	if (_buffering) {
		if (_widgets[type]->_buffer) {
			_bufferQueue.push_back(q);
		} else {
			if (kDrawDataDefaults[type].parent != kDDNone && kDrawDataDefaults[type].parent != type)
				queueDD(kDrawDataDefaults[type].parent, r);

			_screenQueue.push_back(q);
		}
	} else {
		q->drawSelf(!_widgets[type]->_buffer, restore || _widgets[type]->_buffer);
		delete q;
	}
}

void ThemeEngine::queueDDText(TextData type, const Common::Rect &r, const Common::String &text, bool restoreBg,
	bool ellipsis, Graphics::TextAlign alignH, TextAlignVertical alignV, int deltax) {

	if (_texts[type] == 0)
		return;

	Common::Rect area = r;
	area.clip(_screen.w, _screen.h);

	ThemeItemTextData *q = new ThemeItemTextData(this, _texts[type], area, text, alignH, alignV, ellipsis, restoreBg, deltax);

	if (_buffering) {
		_screenQueue.push_back(q);
	} else {
		q->drawSelf(true, false);
		delete q;
	}
}

void ThemeEngine::queueBitmap(const Graphics::Surface *bitmap, const Common::Rect &r, bool alpha) {

	Common::Rect area = r;
	area.clip(_screen.w, _screen.h);

	ThemeItemBitmap *q = new ThemeItemBitmap(this, area, bitmap, alpha);

	if (_buffering) {
		_bufferQueue.push_back(q);
	} else {
		q->drawSelf(true, false);
		delete q;
	}
}



/**********************************************************
 *	Widget drawing functions
 *********************************************************/
void ThemeEngine::drawButton(const Common::Rect &r, const Common::String &str, WidgetStateInfo state, uint16 hints) {
	if (!ready())
		return;

	DrawData dd = kDDButtonIdle;

	if (state == kStateEnabled)
		dd = kDDButtonIdle;
	else if (state == kStateHighlight)
		dd = kDDButtonHover;
	else if (state == kStateDisabled)
		dd = kDDButtonDisabled;

	queueDD(dd, r, 0, hints & WIDGET_CLEARBG);
	queueDDText(getTextData(dd), r, str, false, false, _widgets[dd]->_textAlignH, _widgets[dd]->_textAlignV);
}

void ThemeEngine::drawLineSeparator(const Common::Rect &r, WidgetStateInfo state) {
	if (!ready())
		return;

	queueDD(kDDSeparator, r);
}

void ThemeEngine::drawCheckbox(const Common::Rect &r, const Common::String &str, bool checked, WidgetStateInfo state) {
	if (!ready())
		return;

	Common::Rect r2 = r;
	DrawData dd = kDDCheckboxDefault;

	if (checked)
		dd = kDDCheckboxSelected;

	if (state == kStateDisabled)
		dd = kDDCheckboxDisabled;

	TextData td = (state == kStateHighlight) ? kTextDataHover : getTextData(dd);
	const int checkBoxSize = MIN((int)r.height(), getFontHeight());

	r2.bottom = r2.top + checkBoxSize;
	r2.right = r2.left + checkBoxSize;

	queueDD(dd, r2);

	r2.left = r2.right + checkBoxSize;
	r2.right = r.right;

	queueDDText(td, r2, str, false, false, _widgets[kDDCheckboxDefault]->_textAlignH, _widgets[dd]->_textAlignV);
}

void ThemeEngine::drawSlider(const Common::Rect &r, int width, WidgetStateInfo state) {
	if (!ready())
		return;

	DrawData dd = kDDSliderFull;

	if (state == kStateHighlight)
		dd = kDDSliderHover;
	else if (state == kStateDisabled)
		dd = kDDSliderDisabled;

	Common::Rect r2 = r;
	r2.setWidth(MIN((int16)width, r.width()));
//	r2.top++; r2.bottom--; r2.left++; r2.right--;

	drawWidgetBackground(r, 0, kWidgetBackgroundSlider, kStateEnabled);

	queueDD(dd, r2);
}

void ThemeEngine::drawScrollbar(const Common::Rect &r, int sliderY, int sliderHeight, ScrollbarState scrollState, WidgetStateInfo state) {
	if (!ready())
		return;

	queueDD(kDDScrollbarBase, r);

	Common::Rect r2 = r;
	const int buttonExtra = (r.width() * 120) / 100;

	r2.bottom = r2.top + buttonExtra;
	queueDD(scrollState == kScrollbarStateUp ? kDDScrollbarButtonHover : kDDScrollbarButtonIdle, r2, Graphics::VectorRenderer::kTriangleUp);

	r2.translate(0, r.height() - r2.height());
	queueDD(scrollState == kScrollbarStateDown ? kDDScrollbarButtonHover : kDDScrollbarButtonIdle, r2, Graphics::VectorRenderer::kTriangleDown);

	r2 = r;
	r2.left += 1;
	r2.right -= 1;
	r2.top += sliderY;
	r2.bottom = r2.top + sliderHeight - 1;

	r2.top += r.width() / 5;
	r2.bottom -= r.width() / 5;
	queueDD(scrollState == kScrollbarStateSlider ? kDDScrollbarHandleHover : kDDScrollbarHandleIdle, r2);
}

void ThemeEngine::drawDialogBackground(const Common::Rect &r, DialogBackground bgtype, WidgetStateInfo state) {
	if (!ready())
		return;

	switch (bgtype) {
		case kDialogBackgroundMain:
			queueDD(kDDMainDialogBackground, r);
			break;

		case kDialogBackgroundSpecial:
			queueDD(kDDSpecialColorBackground, r);
			break;

		case kDialogBackgroundPlain:
			queueDD(kDDPlainColorBackground, r);
			break;

		case kDialogBackgroundDefault:
			queueDD(kDDDefaultBackground, r);
			break;
	}
}

void ThemeEngine::drawCaret(const Common::Rect &r, bool erase, WidgetStateInfo state) {
	if (!ready())
		return;

	if (erase) {
		restoreBackground(r);
		addDirtyRect(r);
	} else
		queueDD(kDDCaret, r);
}

void ThemeEngine::drawPopUpWidget(const Common::Rect &r, const Common::String &sel, int deltax, WidgetStateInfo state, Graphics::TextAlign align) {
	if (!ready())
		return;

	DrawData dd = kDDPopUpIdle;

	if (state == kStateEnabled)
		dd = kDDPopUpIdle;
	else if (state == kStateHighlight)
		dd = kDDPopUpHover;
	else if (state == kStateDisabled)
		dd = kDDPopUpDisabled;

	queueDD(dd, r);

	if (!sel.empty()) {
		Common::Rect text(r.left + 3, r.top + 1, r.right - 10, r.bottom);
		queueDDText(getTextData(dd), text, sel, true, false, _widgets[dd]->_textAlignH, _widgets[dd]->_textAlignV, deltax);
	}
}

void ThemeEngine::drawSurface(const Common::Rect &r, const Graphics::Surface &surface, WidgetStateInfo state, int alpha, bool themeTrans) {
	if (!ready())
		return;

	queueBitmap(&surface, r, themeTrans);
}

void ThemeEngine::drawWidgetBackground(const Common::Rect &r, uint16 hints, WidgetBackground background, WidgetStateInfo state) {
	if (!ready())
		return;

	switch (background) {
	case kWidgetBackgroundBorderSmall:
		queueDD(kDDWidgetBackgroundSmall, r);
		break;

	case kWidgetBackgroundEditText:
		queueDD(kDDWidgetBackgroundEditText, r);
		break;

	case kWidgetBackgroundSlider:
		queueDD(kDDWidgetBackgroundSlider, r);
		break;

	default:
		queueDD(kDDWidgetBackgroundDefault, r);
		break;
	}
}

void ThemeEngine::drawTab(const Common::Rect &r, int tabHeight, int tabWidth, const Common::Array<Common::String> &tabs, int active, uint16 hints, int titleVPad, WidgetStateInfo state) {
	if (!ready())
		return;

	const int tabOffset = 2;
	tabWidth -= tabOffset;

	queueDD(kDDTabBackground, Common::Rect(r.left, r.top, r.right, r.top + tabHeight));

	for (int i = 0; i < (int)tabs.size(); ++i) {
		if (i == active)
			continue;

		Common::Rect tabRect(r.left + i * (tabWidth + tabOffset), r.top, r.left + i * (tabWidth + tabOffset) + tabWidth, r.top + tabHeight);
		queueDD(kDDTabInactive, tabRect);
		queueDDText(getTextData(kDDTabInactive), tabRect, tabs[i], false, false, _widgets[kDDTabInactive]->_textAlignH, _widgets[kDDTabInactive]->_textAlignV);
	}

	if (active >= 0) {
		Common::Rect tabRect(r.left + active * (tabWidth + tabOffset), r.top, r.left + active * (tabWidth + tabOffset) + tabWidth, r.top + tabHeight);
		const uint16 tabLeft = active * (tabWidth + tabOffset);
		const uint16 tabRight =  MAX(r.right - tabRect.right, 0);
		queueDD(kDDTabActive, tabRect, (tabLeft << 16) | (tabRight & 0xFFFF));
		queueDDText(getTextData(kDDTabActive), tabRect, tabs[active], false, false, _widgets[kDDTabActive]->_textAlignH, _widgets[kDDTabActive]->_textAlignV);
	}
}

void ThemeEngine::drawText(const Common::Rect &r, const Common::String &str, WidgetStateInfo state, Graphics::TextAlign align, bool inverted, int deltax, bool useEllipsis, FontStyle font) {
	if (!ready())
		return;

	if (inverted) {
		queueDD(kDDTextSelectionBackground, r);
		queueDDText(kTextDataInverted, r, str, false, useEllipsis, align, kTextAlignVCenter, deltax);
		return;
	}

	switch (font) {
		case kFontStyleNormal:
			queueDDText(kTextDataNormalFont, r, str, true, useEllipsis, align, kTextAlignVCenter, deltax);
			return;

		default:
			break;
	}

	switch (state) {
		case kStateDisabled:
			queueDDText(kTextDataDisabled, r, str, true, useEllipsis, align, kTextAlignVCenter, deltax);
			return;

		case kStateHighlight:
			queueDDText(kTextDataHover, r, str, true, useEllipsis, align, kTextAlignVCenter, deltax);
			return;

		case kStateEnabled:
			queueDDText(kTextDataDefault, r, str, true, useEllipsis, align, kTextAlignVCenter, deltax);
			return;
	}
}

void ThemeEngine::drawChar(const Common::Rect &r, byte ch, const Graphics::Font *font, WidgetStateInfo state) {
	if (!ready())
		return;

	Common::Rect charArea = r;
	charArea.clip(_screen.w, _screen.h);

	uint32 color = _overlayFormat.RGBToColor(_texts[kTextDataDefault]->_color.r, _texts[kTextDataDefault]->_color.g, _texts[kTextDataDefault]->_color.b);

	restoreBackground(charArea);
	font->drawChar(&_screen, ch, charArea.left, charArea.top, color);
	addDirtyRect(charArea);
}

void ThemeEngine::debugWidgetPosition(const char *name, const Common::Rect &r) {
	_font->drawString(&_screen, name, r.left, r.top, r.width(), 0xFFFF, Graphics::kTextAlignRight, 0, true);
	_screen.hLine(r.left, r.top, r.right, 0xFFFF);
	_screen.hLine(r.left, r.bottom, r.right, 0xFFFF);
	_screen.vLine(r.left, r.top, r.bottom, 0xFFFF);
	_screen.vLine(r.right, r.top, r.bottom, 0xFFFF);
}



/**********************************************************
 *	Screen/overlay management
 *********************************************************/
void ThemeEngine::updateScreen() {
	if (!_bufferQueue.empty()) {
		_vectorRenderer->setSurface(&_backBuffer);

		for (Common::List<ThemeItem*>::iterator q = _bufferQueue.begin(); q != _bufferQueue.end(); ++q) {
			(*q)->drawSelf(true, false);
			delete *q;
		}

		_vectorRenderer->setSurface(&_screen);
		memcpy(_screen.getBasePtr(0,0), _backBuffer.getBasePtr(0,0), _screen.pitch * _screen.h);
		_bufferQueue.clear();
	}

	if (!_screenQueue.empty()) {
		_vectorRenderer->disableShadows();
		for (Common::List<ThemeItem*>::iterator q = _screenQueue.begin(); q != _screenQueue.end(); ++q) {
			(*q)->drawSelf(true, false);
			delete *q;
		}

		_vectorRenderer->enableShadows();
		_screenQueue.clear();
	}

	renderDirtyScreen();
}

void ThemeEngine::addDirtyRect(Common::Rect r) {
	// Clip the rect to screen coords
	r.clip(_screen.w, _screen.h);

	// If it is empty after clipping, we are done
	if (r.isEmpty())
		return;

	// Check if the new rectangle is contained within another in the list
	Common::List<Common::Rect>::iterator it;
	for (it = _dirtyScreen.begin(); it != _dirtyScreen.end(); ) {
		// If we find a rectangle which fully contains the new one,
		// we can abort the search.
		if (it->contains(r))
			return;

		// Conversely, if we find rectangles which are contained in
		// the new one, we can remove them
		if (r.contains(*it))
			it = _dirtyScreen.erase(it);
		else
			++it;
	}

	// If we got here, we can safely add r to the list of dirty rects.
	_dirtyScreen.push_back(r);
}

void ThemeEngine::renderDirtyScreen() {
	if (_dirtyScreen.empty())
		return;

	Common::List<Common::Rect>::iterator i, j;
	for (i = _dirtyScreen.begin(); i != _dirtyScreen.end(); ++i) {
		_vectorRenderer->copyFrame(_system, *i);
	}

	_dirtyScreen.clear();
}

void ThemeEngine::openDialog(bool doBuffer, ShadingStyle style) {
	if (doBuffer)
		_buffering = true;

	if (style != kShadingNone) {
		_vectorRenderer->applyScreenShading(style);
		addDirtyRect(Common::Rect(0, 0, _screen.w, _screen.h));
	}

	memcpy(_backBuffer.getBasePtr(0,0), _screen.getBasePtr(0,0), _screen.pitch * _screen.h);
	_vectorRenderer->setSurface(&_screen);
}

bool ThemeEngine::createCursor(const Common::String &filename, int hotspotX, int hotspotY, int scale) {
	return true;
}


/**********************************************************
 *	Legacy GUI::Theme support functions
 *********************************************************/

const Graphics::Font *ThemeEngine::getFont(FontStyle font) const {
	return _texts[fontStyleToData(font)]->_fontPtr;
}

int ThemeEngine::getFontHeight(FontStyle font) const {
	return ready() ? _texts[fontStyleToData(font)]->_fontPtr->getFontHeight() : 0;
}

int ThemeEngine::getStringWidth(const Common::String &str, FontStyle font) const {
	return ready() ? _texts[fontStyleToData(font)]->_fontPtr->getStringWidth(str) : 0;
}

int ThemeEngine::getCharWidth(byte c, FontStyle font) const {
	return ready() ? _texts[fontStyleToData(font)]->_fontPtr->getCharWidth(c) : 0;
}

TextData ThemeEngine::getTextData(DrawData ddId) const {
	return _widgets[ddId] ? (TextData)_widgets[ddId]->_textDataId : kTextDataNone;
}


DrawData ThemeEngine::parseDrawDataId(const Common::String &name) const {
	for (int i = 0; i < kDrawDataMAX; ++i)
		if (name.compareToIgnoreCase(kDrawDataDefaults[i].name) == 0)
			return kDrawDataDefaults[i].id;

	return kDDNone;
}

/**********************************************************
 *	External data loading
 *********************************************************/

const Graphics::Font *ThemeEngine::loadFontFromArchive(const Common::String &filename) {
	Common::SeekableReadStream *stream = 0;
	const Graphics::Font *font = 0;

	if (_themeArchive)
		stream = _themeArchive->createReadStreamForMember(filename);
	if (stream) {
		font = Graphics::NewFont::loadFromCache(*stream);
		delete stream;
	}

	return font;
}

const Graphics::Font *ThemeEngine::loadFont(const Common::String &filename) {
	const Graphics::Font *font = 0;
	Common::String cacheFilename = genCacheFilename(filename.c_str());
	Common::File fontFile;

	if (!cacheFilename.empty()) {
		if (fontFile.open(cacheFilename))
			font = Graphics::NewFont::loadFromCache(fontFile);

		if (font)
			return font;

		if ((font = loadFontFromArchive(cacheFilename)))
			return font;
	}

	// normal open
	if (fontFile.open(filename)) {
		font = Graphics::NewFont::loadFont(fontFile);
	}

	if (!font) {
		font = loadFontFromArchive(filename);
	}

	if (font) {
		if (!cacheFilename.empty()) {
			if (!Graphics::NewFont::cacheFontData(*(const Graphics::NewFont*)font, cacheFilename)) {
				warning("Couldn't create cache file for font '%s'", filename.c_str());
			}
		}
	}

	return font;
}

Common::String ThemeEngine::genCacheFilename(const char *filename) {
	Common::String cacheName(filename);
	for (int i = cacheName.size() - 1; i >= 0; --i) {
		if (cacheName[i] == '.') {
			while ((uint)i < cacheName.size() - 1) {
				cacheName.deleteLastChar();
			}

			cacheName += "fcc";
			return cacheName;
		}
	}

	return Common::String();
}


/**********************************************************
 *	Static Theme XML functions
 *********************************************************/

bool ThemeEngine::themeConfigParseHeader(Common::String header, Common::String &themeName) {
	header.trim();

	if (header.empty())
		return false;

	if (header[0] != '[' || header.lastChar() != ']')
		return false;

	header.deleteChar(0);
	header.deleteLastChar();

	Common::StringTokenizer tok(header, ":");

	if (tok.nextToken() != SCUMMVM_THEME_VERSION_STR)
		return false;

	themeName = tok.nextToken();
	Common::String author = tok.nextToken();

	return tok.empty();
}

bool ThemeEngine::themeConfigUsable(const Common::FSNode &node, Common::String &themeName) {
	Common::File stream;
	bool foundHeader = false;

	if (node.getName().hasSuffix(".zip") && !node.isDirectory()) {
#ifdef USE_ZLIB
		Common::ZipArchive zipArchive(node);
		if (zipArchive.hasFile("THEMERC")) {
			stream.open("THEMERC", zipArchive);
		}
#endif
	} else if (node.isDirectory()) {
		Common::FSNode headerfile = node.getChild("THEMERC");
		if (!headerfile.exists() || !headerfile.isReadable() || headerfile.isDirectory())
			return false;
		stream.open(headerfile);
	}

	if (stream.isOpen()) {
		Common::String stxHeader = stream.readLine();
		foundHeader = themeConfigParseHeader(stxHeader, themeName);
	}

	return foundHeader;
}

namespace {

struct TDComparator {
	const Common::String _id;
	TDComparator(const Common::String &id) : _id(id) {}

	bool operator()(const ThemeEngine::ThemeDescriptor &r) { return _id == r.id; }
};

} // end of anonymous namespace

void ThemeEngine::listUsableThemes(Common::List<ThemeDescriptor> &list) {
#ifdef GUI_ENABLE_BUILTIN_THEME
	ThemeDescriptor th;
	th.name = "ScummVM Classic Theme (Builtin Version)";
	th.id = "builtin";
	th.filename.clear();
	list.push_back(th);
#endif

	if (ConfMan.hasKey("themepath"))
		listUsableThemes(Common::FSNode(ConfMan.get("themepath")), list);

#ifdef DATA_PATH
	listUsableThemes(Common::FSNode(DATA_PATH), list);
#endif

#if defined(MACOSX) || defined(IPHONE)
	CFURLRef resourceUrl = CFBundleCopyResourcesDirectoryURL(CFBundleGetMainBundle());
	if (resourceUrl) {
		char buf[256];
		if (CFURLGetFileSystemRepresentation(resourceUrl, true, (UInt8 *)buf, 256)) {
			Common::FSNode resourcePath(buf);
			listUsableThemes(resourcePath, list);
		}
		CFRelease(resourceUrl);
	}
#endif

	if (ConfMan.hasKey("extrapath"))
		listUsableThemes(Common::FSNode(ConfMan.get("extrapath")), list);

	listUsableThemes(Common::FSNode("."), list, 1);

	// Now we need to strip all duplicates
	// TODO: It might not be the best idea to strip duplicates. The user might
	// have different versions of a specific theme in his paths, thus this code
	// might show him the wrong version. The problem is we have no ways of checking
	// a theme version currently. Also since we want to avoid saving the full path
	// in the config file we can not do any better currently.
	Common::List<ThemeDescriptor> output;

	for (Common::List<ThemeDescriptor>::const_iterator i = list.begin(); i != list.end(); ++i) {
		if (Common::find_if(output.begin(), output.end(), TDComparator(i->id)) == output.end())
			output.push_back(*i);
	}

	list = output;
	output.clear();
}

void ThemeEngine::listUsableThemes(Common::FSNode node, Common::List<ThemeDescriptor> &list, int depth) {
	if (!node.exists() || !node.isReadable() || !node.isDirectory())
		return;

	ThemeDescriptor td;

	// Check whether we point to a valid theme directory.
	if (themeConfigUsable(node, td.name)) {
		td.filename = node.getPath();
		td.id = node.getName();

		list.push_back(td);

		// A theme directory should never contain any other themes
		// thus we just return to the caller here.
		return;
	}

	Common::FSList fileList;
#ifdef USE_ZLIB
	// Check all files. We need this to find all themes inside ZIP archives.
	if (!node.getChildren(fileList, Common::FSNode::kListFilesOnly))
		return;

	for (Common::FSList::iterator i = fileList.begin(); i != fileList.end(); ++i) {
		// We will only process zip files for now
		if (!i->getPath().hasSuffix(".zip"))
			continue;

		td.name.clear();
		if (themeConfigUsable(*i, td.name)) {
			td.filename = i->getPath();
			td.id = i->getName();

			// If the name of the node object also contains
			// the ".zip" suffix, we will strip it.
			if (td.id.hasSuffix(".zip")) {
				for (int j = 0; j < 4; ++j)
					td.id.deleteLastChar();
			}

			list.push_back(td);
		}
	}

	fileList.clear();
#endif

	// Check if we exceeded the given recursion depth
	if (depth - 1 == -1)
		return;

	// As next step we will search all subdirectories
	if (!node.getChildren(fileList, Common::FSNode::kListDirectoriesOnly))
		return;

	for (Common::FSList::iterator i = fileList.begin(); i != fileList.end(); ++i)
		listUsableThemes(*i, list, depth == -1 ? - 1 : depth - 1);
}

Common::String ThemeEngine::getThemeFile(const Common::String &id) {
	// FIXME: Actually "default" rather sounds like it should use
	// our default theme which would mean "scummmodern" instead
	// of the builtin one.
	if (id.equalsIgnoreCase("default"))
		return Common::String();

	// For our builtin theme we don't have to do anything for now too
	if (id.equalsIgnoreCase("builtin"))
		return Common::String();

	Common::FSNode node(id);

	// If the given id is a full path we'll just use it
	if (node.exists() && (node.isDirectory() || node.getName().hasSuffix(".zip")))
		return id;

	// FIXME:
	// A very ugly hack to map a id to a filename, this will generate
	// a complete theme list, thus it is slower than it could be.
	// But it is the easiest solution for now.
	Common::List<ThemeDescriptor> list;
	listUsableThemes(list);

	for (Common::List<ThemeDescriptor>::const_iterator i = list.begin(); i != list.end(); ++i) {
		if (id.equalsIgnoreCase(i->id))
			return i->filename;
	}

	warning("Could not find theme '%s' falling back to builtin", id.c_str());

	// If no matching id has been found we will
	// just fall back to the builtin theme
	return Common::String();
}

Common::String ThemeEngine::getThemeId(const Common::String &filename) {
	// If no filename has been given we will initialize the builtin theme
	if (filename.empty())
		return "builtin";

	Common::FSNode node(filename);
	if (!node.exists())
		return "builtin";

	if (node.getName().hasSuffix(".zip")) {
		Common::String id = node.getName();

		for (int i = 0; i < 4; ++i)
			id.deleteLastChar();

		return id;
	} else {
		return node.getName();
	}
}


} // end of namespace GUI.