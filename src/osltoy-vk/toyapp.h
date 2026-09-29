// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

// The osltoy-vk main window. Adapted from osltoy's OSLToyMainWindow, with a
// Vulkan 3D viewport instead of the CPU-rendered image, a read-only tab
// showing the generated Slang, and a panel of build and render statistics.

#include <memory>
#include <unordered_map>

#include <OpenImageIO/paramlist.h>
#include <OpenImageIO/timer.h>

#include <OSL/oslconfig.h>
#include <OSL/oslquery.h>

#include <QAction>
#include <QMainWindow>
#include <QTimer>

#include "toyrenderer.h"

class QGridLayout;
class QLabel;
class QMenu;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTabWidget;
class QTextEdit;
class QVulkanInstance;
class CodeEditor;

namespace osltoyvk {
class VkView;
}

OSL_NAMESPACE_BEGIN

class ParamRec final : public OSLQuery::Parameter {
public:
    using Parameter = OSLQuery::Parameter;
    std::vector<QWidget*> widgets;
    ustring layername;

    ParamRec() {}
    ParamRec(const Parameter& p) : Parameter(p) {}
};

class ToyMainWindow final : public QMainWindow {
    Q_OBJECT

public:
    ToyMainWindow(ToyRenderer* rend, QVulkanInstance* inst, int xres,
                  int yres);
    ~ToyMainWindow();

    void recompile_shaders();
    void build_shader_group();
    void toggle_pause();
    void restart_time();

    bool open_file(const std::string& filename);
    void set_include_search_paths(const std::vector<std::string>& paths)
    {
        m_include_search_paths = paths;
    }

    CodeEditor* add_new_editor_window(const std::string& filename = "");
    void set_error_message(int tab, const std::string& msg = "");
    int ntabs() const;

    ToyRenderer* renderer() const { return m_renderer.get(); }

    /// For --check: did the last build succeed, and a text report of it
    /// (errors and statistics).
    bool build_ok() const { return m_build_ok; }
    std::string build_report() const;
    /// Diddle the named parameter, as its checkbox does (for testing).
    void diddle(const std::string& name);
    /// Run a test script of semicolon-separated actions:
    ///   diddle NAME | undiddle NAME | set NAME VALUE | recompile
    void run_script(const std::string& script);
    /// Once the viewport has drawn a few frames, save it to `filename`,
    /// print the build report, and quit.
    void snapshot_and_quit(const std::string& filename);
    ShadingSystem* shadingsys() const { return m_renderer->shadingsys(); }

private:
    void createActions();
    void createMenus();
    void action_newfile() { add_new_editor_window(); }
    void action_open();
    void action_save();
    void action_saveas();

    // Build and hand the viewport a new program for the current group
    void rebuild_program();
    void update_stats();
    void show_messages(const std::vector<uint32_t>& words);
    void tick();

    void inventory_params();
    void rebuild_param_area();
    void clear_param_area();
    void make_param_adjustment_row(ParamRec* param, QGridLayout* layout,
                                   int row);
    void set_ui_to_paramval(ParamRec* param);
    void reset_param_to_default(ParamRec* param);
    void set_param_instance_value(ParamRec* param);
    void set_param_diddle(ParamRec* param, int state);

    template<typename ACT>
    QAction* add_action(const std::string& name, const std::string& label,
                        const std::string& hotkey, ACT trigger_action)
    {
        QAction* act = new QAction(label.size() ? label.c_str() : name.c_str(),
                                   this);
        actions[name] = act;
        if (hotkey.size())
            act->setShortcut(QString(hotkey.c_str()));
        connect(act, &QAction::triggered, this, trigger_action);
        return act;
    }
    std::unordered_map<std::string, QAction*> actions;

    std::unique_ptr<ToyRenderer> m_renderer;
    osltoyvk::VkView* m_view = nullptr;

    QTabWidget* textTabs       = nullptr;
    CodeEditor* slangEditor    = nullptr;
    QScrollArea* paramScroll   = nullptr;
    QWidget* paramWidget       = nullptr;
    QGridLayout* paramLayout   = nullptr;
    QLabel* statsLabel         = nullptr;
    QTextEdit* outputDisplay   = nullptr;
    QPushButton* pauseButton   = nullptr;
    std::vector<CodeEditor*> editors;
    std::vector<QTextEdit*> error_displays;
    QTimer* maintimer = nullptr;

    std::vector<std::shared_ptr<ParamRec>> m_shaderparams;
    OIIO::ParamValueList m_shaderparam_instvalues;
    std::unordered_map<std::string, bool> m_diddlers;
    std::string m_firstshadername;
    ustring m_selectedoutput;
    bool m_show_output = false;  // show m_selectedoutput instead of Ci
    std::vector<std::string> m_include_search_paths;

    ToyRenderer::Program m_program;
    double m_oslc_seconds = 0;
    bool m_want_messages  = false;
    OIIO::Timer m_timer { false };
    bool m_paused = true;
    bool m_build_ok = false;
    int m_build_count = 0;  // (for testing)
    std::string m_build_error;
};

OSL_NAMESPACE_END
