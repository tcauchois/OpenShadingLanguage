// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// Adapted from osltoy's osltoyapp.cpp.

#include "toyapp.h"
#include "codeeditor.h"

#include <iostream>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QErrorMessage>
#include <QFileDialog>
#include <QFontDatabase>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QVulkanInstance>

// QT's extension foreach defines a foreach macro which interferes
// with an OSL internal foreach method.  So we will undefine it here
#undef foreach

#include <OpenImageIO/errorhandler.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/strutil.h>
#include <OpenImageIO/sysutil.h>

#include <OSL/oslcomp.h>
#include <OSL/oslexec.h>

#include "qtutils.h"
#include "vkview.h"


OSL_NAMESPACE_BEGIN

namespace {

// The shader in a new window: a simple material, with a procedural checker
// to show off per-pixel OSL.
const char* default_shader = R"(// A simple material for osltoy-vk. The viewport lights the
// diffuse, microfacet, and emission closures. Check "diddle" next to a
// parameter to adjust it interactively.

surface default_material(
    color base_color = color(0.8, 0.3, 0.2),
    float roughness = 0.35,
    float metallic = 0,
    color emissive = 0,
    float checker_scale = 8)
{
    float chk = (int(floor(u * checker_scale * 2))
                 + int(floor(v * checker_scale))) % 2;
    color albedo = mix(base_color, base_color * 0.4, chk);
    float alpha = roughness * roughness;
    Ci = (1 - metallic) * albedo * diffuse(N)
       + mix(color(1), albedo, metallic)
             * microfacet("ggx", N, vector(0), alpha, alpha, 1.5, 0)
       + emissive * emission();
}
)";

class OslcErrors final : public OIIO::ErrorHandler {
public:
    void operator()(int, const std::string& msg) override
    {
        errors.emplace_back(msg);
    }
    std::vector<std::string> errors;
};

std::string
human_bytes(size_t b)
{
    if (b < 4096)
        return fmtformat("{} B", b);
    if (b < 4 * 1024 * 1024)
        return fmtformat("{:.1f} KB", b / 1024.0);
    return fmtformat("{:.1f} MB", b / (1024.0 * 1024.0));
}

}  // namespace



ToyMainWindow::ToyMainWindow(ToyRenderer* rend, QVulkanInstance* inst,
                             int xres, int yres)
    : QMainWindow(nullptr), m_renderer(rend)
{
    setWindowTitle(tr("OSL Toy (Vulkan)"));
    createActions();
    createMenus();

    // The viewport
    m_view = new osltoyvk::VkView;
    m_view->setVulkanInstance(inst);
    QWidget* viewwidget = QWidget::createWindowContainer(m_view, this);
    viewwidget->setMinimumSize(xres, yres);
    m_view->on_messages = [this](std::vector<uint32_t> w) { show_messages(w); };
    m_view->on_error    = [this](std::string e) {
        outputDisplay->setTextColor(Qt::red);
        outputDisplay->setPlainText(e.c_str());
    };

    statsLabel = new QLabel;
    QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    statsLabel->setFont(fixedFont);
    statsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    outputDisplay = new QTextEdit;
    outputDisplay->setReadOnly(true);
    outputDisplay->setFixedHeight(70);
    outputDisplay->setPlaceholderText(
        "Shader output (printf/errors from the pixel at the view's center)");

    clear_param_area();
    paramLayout->addWidget(
        new QLabel("Compile a shader to see its parameters."), 0, 0);
    paramScroll->setWidget(paramWidget);
    paramScroll->setWidgetResizable(true);

    auto display_area        = new QWidget;
    auto display_area_layout = new QVBoxLayout;
    display_area->setLayout(display_area_layout);
    display_area_layout->addWidget(viewwidget, 1);
    display_area_layout->addWidget(statsLabel);
    display_area_layout->addWidget(outputDisplay);

    // The editors: OSL source tabs, then the generated Slang (read-only)
    textTabs = new QTabWidget;
    add_new_editor_window();
    editors[0]->setPlainText(default_shader);
    editors[0]->set_filename("default_material.osl");
    textTabs->setTabText(0, "default_material.osl");

    slangEditor = new CodeEditor(nullptr, "generated.slang");
    slangEditor->setReadOnly(true);
    slangEditor->setPlainText("// Compile a shader to see the Slang code "
                              "generated for it.");
    textTabs->addTab(slangEditor, "Slang (generated)");
    // The parameter controls get their own tab, so they have room.
    textTabs->addTab(paramScroll, "Parameters");
    textTabs->setCurrentIndex(0);

    auto control_area        = new QWidget;
    auto control_area_layout = new QHBoxLayout;
    control_area->setLayout(control_area_layout);
    auto recompileButton = new QPushButton("Recompile");
    connect(recompileButton, &QPushButton::clicked, this,
            &ToyMainWindow::recompile_shaders);
    control_area_layout->addWidget(recompileButton);
    pauseButton = new QPushButton("Pause");
    connect(pauseButton, &QPushButton::clicked, this,
            &ToyMainWindow::toggle_pause);
    control_area_layout->addWidget(pauseButton);
    auto restartButton = new QPushButton("Restart");
    connect(restartButton, &QPushButton::clicked, this,
            &ToyMainWindow::restart_time);
    control_area_layout->addWidget(restartButton);
    auto printButton = new QPushButton("Show printf");
    printButton->setToolTip(
        "Show messages (printf, errors) from the pixel at the view's center");
    connect(printButton, &QPushButton::clicked, this, [this] {
        m_view->request_messages();
        m_view->requestUpdate();
    });
    control_area_layout->addWidget(printButton);
    auto continuousBox = new QCheckBox("Continuous");
    continuousBox->setToolTip("Render continuously, to measure the frame "
                              "rate");
    connect(continuousBox, &QCheckBox::toggled, this,
            [this](bool on) { m_view->set_continuous(on); });
    control_area_layout->addWidget(continuousBox);

    auto editorarea = new QWidget;
    QFontMetrics fontmetrics(CodeEditor::fixedFont());
    editorarea->setMinimumSize(85 * fontmetrics.horizontalAdvance(
                                   QLatin1Char('M')),
                               40 * fontmetrics.lineSpacing());
    auto editorarea_layout = new QVBoxLayout;
    editorarea->setLayout(editorarea_layout);
    editorarea_layout->addWidget(textTabs);
    editorarea_layout->addWidget(control_area);

    auto splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(display_area);
    splitter->addWidget(editorarea);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    maintimer = new QTimer(this);
    maintimer->setInterval(500);
    connect(maintimer, &QTimer::timeout, this, &ToyMainWindow::tick);
    maintimer->start();
    update_stats();
}



ToyMainWindow::~ToyMainWindow() {}



void
ToyMainWindow::createActions()
{
    add_action("Exit", "E&xit", "Ctrl+Q", &ToyMainWindow::close);
    add_action("New file", "", "Ctrl+N", &ToyMainWindow::action_newfile);
    add_action("Open...", "", "Ctrl+O", &ToyMainWindow::action_open);
    add_action("Save", "", "Ctrl+S", &ToyMainWindow::action_save);
    add_action("Save As...", "", "Shift-Ctrl+S", &ToyMainWindow::action_saveas);
    add_action("Recompile shaders", "", "Ctrl+R",
               &ToyMainWindow::recompile_shaders);
}



void
ToyMainWindow::createMenus()
{
    QMenu* fileMenu = new QMenu(tr("&File"), this);
    fileMenu->addAction(actions["New file"]);
    fileMenu->addAction(actions["Open..."]);
    fileMenu->addAction(actions["Save"]);
    fileMenu->addAction(actions["Save As..."]);
    fileMenu->addSeparator();
    fileMenu->addAction(actions["Exit"]);
    menuBar()->addMenu(fileMenu);
    QMenu* toolsMenu = new QMenu(tr("&Tools"), this);
    toolsMenu->addAction(actions["Recompile shaders"]);
    menuBar()->addMenu(toolsMenu);
}



int
ToyMainWindow::ntabs() const
{
    return int(editors.size());
}



CodeEditor*
ToyMainWindow::add_new_editor_window(const std::string& filename)
{
    auto texteditor = new CodeEditor(nullptr, filename);
    editors.push_back(texteditor);
    auto errdisplay = new QTextEdit;
    errdisplay->setReadOnly(true);
    errdisplay->setFixedHeight(50);
    errdisplay->setPlainText("Not yet compiled");
    error_displays.push_back(errdisplay);
    auto both   = new QWidget;
    auto layout = new QVBoxLayout;
    both->setLayout(layout);
    layout->addWidget(texteditor);
    layout->addWidget(errdisplay);
    // (OSL tabs come before the Slang tab.)
    int n            = ntabs() - 1;
    std::string name = filename.size() ? texteditor->brief_filename()
                                       : fmtformat("untitled {}", n + 1);
    textTabs->insertTab(n, both, name.c_str());
    textTabs->setCurrentIndex(n);
    return texteditor;
}



void
ToyMainWindow::set_error_message(int tab, const std::string& msg)
{
    if (msg.size()) {
        error_displays[tab]->setTextColor(Qt::red);
        error_displays[tab]->setPlainText(msg.c_str());
    } else {
        error_displays[tab]->setTextColor(Qt::darkGreen);
        error_displays[tab]->setPlainText("Ok");
    }
}



static const char* s_file_filters = "Shaders (*.osl);;All Files (*)";

void
ToyMainWindow::action_open()
{
    QStringList files = QFileDialog::getOpenFileNames(
        nullptr, "Select one or more files to open", QString(), s_file_filters,
        nullptr, QFileDialog::Option::DontUseNativeDialog);
    for (auto& name : files)
        open_file(OIIO::Strutil::to_string(name));
}



bool
ToyMainWindow::open_file(const std::string& filename)
{
    std::string contents;
    if (!OIIO::Filesystem::read_text_file(filename, contents))
        return false;
    int tab = textTabs->currentIndex();
    CodeEditor* texteditor = tab < ntabs() ? editors[tab] : nullptr;
    // Replace the default (unmodified) shader or an empty tab.
    bool replace = texteditor
                   && (texteditor->text_string().empty()
                       || texteditor->text_string() == default_shader);
    if (!replace)
        texteditor = add_new_editor_window(filename);
    texteditor->set_filename(filename);
    texteditor->setPlainText(contents.c_str());
    textTabs->setTabText(textTabs->indexOf(texteditor->parentWidget()),
                         texteditor->brief_filename().c_str());
    return true;
}



void
ToyMainWindow::action_saveas()
{
    int tab = textTabs->currentIndex();
    if (tab >= ntabs())
        return;
    QString name = QFileDialog::getSaveFileName(
        nullptr, "Save buffer", editors[tab]->full_filename().c_str(),
        s_file_filters, nullptr, QFileDialog::Option::DontUseNativeDialog);
    if (name.isEmpty())
        return;
    editors[tab]->set_filename(name.toUtf8().data());
    textTabs->setTabText(tab, editors[tab]->brief_filename().c_str());
    action_save();
}



void
ToyMainWindow::action_save()
{
    int tab = textTabs->currentIndex();
    if (tab >= ntabs())
        return;
    std::string filename = editors[tab]->full_filename();
    if (filename == "untitled" || filename.empty()
        || !OIIO::Filesystem::exists(OIIO::Filesystem::parent_path(filename)))
        return action_saveas();
    OIIO::ofstream out;
    OIIO::Filesystem::open(out, filename);
    if (out)
        out << editors[tab]->text_string();
    if (out.fail()) {
        QErrorMessage err(nullptr);
        err.showMessage(fmtformat("Could not write {}", filename).c_str());
        err.exec();
    }
}



void
ToyMainWindow::recompile_shaders()
{
    m_firstshadername.clear();
    QtUtils::clear_layout(paramLayout);
    bool ok = false;
    for (int tab = 0; tab < ntabs(); ++tab) {
        CodeEditor* editor     = editors[tab];
        std::string briefname  = editor->brief_filename();
        std::string shadername = OIIO::Filesystem::filename(briefname);
        if (!OIIO::Strutil::ends_with(briefname, ".osl"))
            continue;
        OslcErrors errhandler;
        OSLCompiler oslcomp(&errhandler);
        std::vector<std::string> options;
        for (auto&& path : m_include_search_paths)
            options.push_back("-I" + path);
#ifdef OSLTOYVK_SOURCE_SHADERS
        // Last resort for stdosl.h, when running from the build tree.
        options.push_back("-I" OSLTOYVK_SOURCE_SHADERS);
#endif
        std::string oso;
        OIIO::Timer timer;
        ok             = oslcomp.compile_buffer(editor->text_string(), oso,
                                                options, "", briefname);
        m_oslc_seconds = timer();
        set_error_message(tab,
                          OIIO::Strutil::join(errhandler.errors, "\n"));
        if (!ok) {
            textTabs->setCurrentIndex(tab);
            break;
        }
        ok = shadingsys()->LoadMemoryCompiledShader(briefname, oso);
        m_firstshadername = shadername;
        break;  // only one shader for now
    }
    if (ok) {
        build_shader_group();
        inventory_params();
        rebuild_param_area();
        if (m_paused)
            toggle_pause();
        textTabs->setTabText(textTabs->indexOf(paramScroll),
                             fmtformat("Parameters ({})", m_shaderparams.size())
                                 .c_str());
    }
}



void
ToyMainWindow::build_shader_group()
{
    ShadingSystem* ss = shadingsys();
    if (m_firstshadername.empty())
        return;
    ShaderGroupRef group = ss->ShaderGroupBegin();
    // Diddled params are "interactive", so that ReParameter can change them
    // without recompiling; they need a value to carry that hint, so give
    // them their current (or default) value.
    for (auto&& p : m_shaderparams) {
        if (!m_diddlers[p->name.string()] || p->isoutput
            || m_shaderparam_instvalues.find(p->name)
                   != m_shaderparam_instvalues.end())
            continue;
        if (p->type == TypeDesc::INT)
            m_shaderparam_instvalues.push_back(
                OIIO::ParamValue(p->name, p->type, 1, p->idefault.data()));
        else if (p->type == TypeDesc::FLOAT || p->type.is_vec3())
            m_shaderparam_instvalues.push_back(
                OIIO::ParamValue(p->name, p->type, 1, p->fdefault.data()));
        else if (p->type == TypeDesc::STRING)
            m_shaderparam_instvalues.push_back(
                OIIO::ParamValue(p->name, p->type, 1, p->sdefault.data()));
    }
    for (auto&& instparam : m_shaderparam_instvalues)
        ss->Parameter(*group, instparam.name(), instparam.type(),
                      instparam.data(),
                      m_diddlers[instparam.name().string()]
                          ? ParamHints::interactive
                          : ParamHints::none);
    ss->Shader("surface", m_firstshadername, "layer1");
    ss->ShaderGroupEnd();

    // Choose an output to show if the shader doesn't set Ci: the first
    // color output, unless the user picked another.
    OSLQuery q = ss->oslquery(*group, 0);
    bool selected_ok = false;
    ustring first_output;
    for (size_t p = 0; p < q.nparams(); ++p) {
        auto param = q.getparam(p);
        if (param->isoutput && param->type.is_vec3()) {
            if (first_output.empty())
                first_output = param->name;
            selected_ok |= param->name == m_selectedoutput;
        }
    }
    if (!selected_ok)
        m_selectedoutput = first_output;
    renderer()->set_display_output(m_selectedoutput);
    renderer()->set_shadergroup(group);
    rebuild_program();
}



void
ToyMainWindow::rebuild_program()
{
    std::string err;
    ToyRenderer::Program program;
    ++m_build_count;
    bool ok    = renderer()->build_program(program, err);
    m_build_ok = ok;
    m_build_error = err;
    if (!ok) {
        outputDisplay->setTextColor(Qt::red);
        outputDisplay->setPlainText(
            ("Shader group build failed:\n" + err).c_str());
        return;
    }
    outputDisplay->clear();
    m_program = program;
    slangEditor->setPlainText(program.fragment_source.c_str());

    // Show the selected output if the shader doesn't set Ci.
    bool has_closures = program.stats.closure_pool_bytes > 0;
    m_view->set_display_output(m_show_output || !has_closures);

    osltoyvk::ShaderProgram sp;
    sp.vertex   = program.vertex;
    sp.fragment = program.fragment;
    sp.params   = renderer()->interactive_params();
    sp.animate  = program.uses_time;
    m_view->set_program(sp);
    update_stats();
}



void
ToyMainWindow::show_messages(const std::vector<uint32_t>& words)
{
    std::string text = renderer()->decode_messages(words.data(), words.size());
    outputDisplay->setTextColor(Qt::black);
    outputDisplay->setPlainText(text.size()
                                    ? text.c_str()
                                    : "(no messages from the center pixel)");
}



void
ToyMainWindow::tick()
{
    if (!m_paused)
        m_view->set_time(float(m_timer()));
    update_stats();
}



std::string
ToyMainWindow::build_report() const
{
    std::string errs;
    for (QTextEdit* e : error_displays)
        errs += OIIO::Strutil::to_string(e->toPlainText()) + "\n";
    return errs + (m_build_ok ? "" : "Build failed: " + m_build_error + "\n")
           + OIIO::Strutil::to_string(statsLabel->text());
}



void
ToyMainWindow::snapshot_and_quit(const std::string& filename)
{
    // (Let the viewport create its swapchain and draw frames for a while,
    // to measure the frame rate, with a message readback.)
    m_view->set_continuous(true);
    if (OIIO::Sysutil::getenv("OSLTOYVK_TEST_HIDE").size()) {
        // Test recovery from the window being hidden (as when minimized).
        QTimer::singleShot(700, this, [this] { showMinimized(); });
        QTimer::singleShot(1200, this, [this] {
            showNormal();
            raise();
            activateWindow();
        });
    }
    QTimer::singleShot(1500, this, [this] {
        m_view->request_messages();
        m_view->requestUpdate();
    });
    int wait = OIIO::Sysutil::getenv("OSLTOYVK_TEST_HIDE").size() ? 5000 : 3000;
    QTimer::singleShot(wait, this, [this, filename] {
        QImage image = m_view->snapshot();
        osltoyvk::ViewStats vs = m_view->stats();
        if (OIIO::Sysutil::getenv("OSLTOYVK_TEST_HIDE").size())
            std::cout << "Viewport: exposed " << m_view->isExposed()
                      << ", valid " << m_view->isValid() << ", frames "
                      << vs.frames << ", resource inits " << vs.resource_inits
                      << ", pipeline " << vs.pipeline_ready << "\n";
        bool ok = !image.isNull() && image.save(filename.c_str());
        // Also save the window (without the Vulkan viewport, which grab()
        // can't capture), showing the parameters tab.
        textTabs->setCurrentWidget(paramScroll);
        std::string wname = OIIO::Filesystem::replace_extension(filename, "")
                            + "_window.png";
        grab().save(wname.c_str());
        update_stats();
        std::cout << build_report() << "\n"
                  << "Shader output: "
                  << OIIO::Strutil::to_string(outputDisplay->toPlainText())
                  << "\n"
                  << (ok ? "Saved " : "Could not save ") << filename << "\n";
        QApplication::exit(ok && m_build_ok ? 0 : 1);
    });
}



void
ToyMainWindow::update_stats()
{
    const ToyRenderer::BuildStats& b = m_program.stats;
    osltoyvk::ViewStats v            = m_view->stats();
    auto ms = [](double s) { return fmtformat("{:7.1f} ms", s * 1000); };
    std::string gpu = v.gpu_ms >= 0
                          ? fmtformat("{:.2f} ms", v.gpu_ms)
                          : std::string("n/a (no timestamp queries)");
    std::string rate = v.fps > 0 ? fmtformat("{:6.1f} fps ({:.2f} ms/frame)",
                                             v.fps, v.frame_ms)
                                 : std::string("idle (draws on change; check "
                                               "Continuous to time it)");
    std::string text = fmtformat(
        "Device: {}\n"
        "Build:  oslc {}  | OSL->SPIR-V {} (optimize+LLVM {}, of which LLVM {})\n"
        "        Slang {}  | link+validate {}  | pipeline {}\n"
        "Render: {}  GPU {}\n"
        "SPIR-V: OSL {}  Slang {}  linked {}  "
        "(code {}, constants {}, other {}; eval_osl {} instrs)\n"
        "Memory per lane: groupdata {} (closure pool {}), "
        "Function vars {}, Private vars {}",
        v.device, ms(m_oslc_seconds), ms(b.osl_to_spirv), ms(b.opt_time),
        ms(b.llvm_time), ms(b.slang_compile), ms(b.link_validate),
        ms(v.pipeline_seconds), rate, gpu,
        human_bytes(4 * b.osl_words), human_bytes(4 * b.slang_words),
        human_bytes(4 * b.linked_words), human_bytes(4 * b.code_words),
        human_bytes(4 * b.constant_words), human_bytes(4 * b.other_words),
        b.eval_osl_instructions, human_bytes(b.groupdata_bytes),
        human_bytes(b.closure_pool_bytes), human_bytes(b.function_var_bytes),
        human_bytes(b.private_var_bytes));
    statsLabel->setText(text.c_str());
}



void
ToyMainWindow::toggle_pause()
{
    m_paused = !m_paused;
    if (m_paused) {
        pauseButton->setText("Continue");
        m_timer.stop();
    } else {
        pauseButton->setText("Pause");
        m_timer.start();
    }
    m_view->set_paused(m_paused);
    m_view->requestUpdate();
}



void
ToyMainWindow::restart_time()
{
    m_timer.reset();
    if (!m_paused)
        m_timer.start();
    m_view->set_time(0);
    m_view->requestUpdate();
}



// Parameter controls (as osltoy's)

void
ToyMainWindow::clear_param_area()
{
    if (paramScroll)
        delete paramScroll->takeWidget();
    else
        paramScroll = new QScrollArea;
    paramWidget = new QWidget;
    paramLayout = new QGridLayout;
    paramLayout->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    paramLayout->setColumnStretch(1, 0);
    paramWidget->setLayout(paramLayout);
}



void
ToyMainWindow::inventory_params()
{
    ShadingSystem* ss    = shadingsys();
    ShaderGroupRef group = renderer()->shadergroup();
    if (!group)
        return;
    int nlayers = 0;
    ss->getattribute(group.get(), "num_layers", nlayers);
    std::vector<ustring> layernames(nlayers);
    ss->getattribute(group.get(), "layer_names",
                     TypeDesc(TypeDesc::STRING, nlayers), &layernames[0]);
    m_shaderparams.clear();
    for (int i = 0; i < nlayers; ++i) {
        OSLQuery oslquery = ss->oslquery(*group, i);
        for (size_t p = 0; p < oslquery.nparams(); ++p) {
            m_shaderparams.push_back(
                std::make_shared<ParamRec>(*oslquery.getparam(p)));
            m_shaderparams.back()->layername = layernames[i];
        }
    }
}



void
ToyMainWindow::rebuild_param_area()
{
    ShaderGroupRef group = renderer()->shadergroup();
    if (!group)
        return;
    clear_param_area();
    int row = 0;
    auto header = new QLabel(
        fmtformat("Shader <b>{}</b>. Check the box next to a parameter to "
                  "adjust it interactively, without recompiling (\"diddle\" "
                  "it).",
                  m_firstshadername)
            .c_str());
    header->setWordWrap(true);
    paramLayout->addWidget(header, row++, 0, 1, 4);
    if (m_program.stats.closure_pool_bytes > 0) {
        // A shader with closures: lit by default, or show an output.
        auto showOutput = new QCheckBox("Show the selected output instead of "
                                        "lighting Ci");
        showOutput->setChecked(m_show_output);
        connect(showOutput, &QCheckBox::toggled, this, [this](bool on) {
            m_show_output = on;
            m_view->set_display_output(on);
            m_view->requestUpdate();
        });
        paramLayout->addWidget(showOutput, row++, 0, 1, 3);
    }
    for (auto&& p : m_shaderparams)
        make_param_adjustment_row(p.get(), paramLayout, row++);
    paramScroll->setWidget(paramWidget);
}



void
ToyMainWindow::make_param_adjustment_row(ParamRec* param, QGridLayout* layout,
                                         int row)
{
    std::string typetext(param->type.c_str());
    if (param->isclosure)
        typetext = fmtformat("closure {}", typetext);
    if (param->isstruct)
        typetext = fmtformat("struct {}", param->structname);
    if (param->isoutput)
        typetext = fmtformat("output {}", typetext);
    auto nameLabel = new QLabel(
        fmtformat("<i>{}</i>&nbsp;  <b>{}</b>", typetext, param->name).c_str());
    nameLabel->setTextFormat(Qt::RichText);

    if (param->isoutput) {
        if (param->type.is_vec3()) {
            // Choose the output to show
            auto radio = new QRadioButton(this);
            layout->addWidget(radio, row, 0);
            radio->setChecked(m_selectedoutput == param->name);
            connect(radio, &QRadioButton::toggled, this,
                    [this, param](bool checked) {
                        if (checked && m_selectedoutput != param->name) {
                            m_selectedoutput = param->name;
                            renderer()->set_display_output(m_selectedoutput);
                            rebuild_program();
                        }
                    });
        }
        layout->addWidget(nameLabel, row, 1);
        return;
    }

    auto diddle = new QCheckBox("  ");
    diddle->setToolTip("Adjust interactively (without recompiling)");
    if (m_diddlers[param->name.string()])
        diddle->setCheckState(Qt::Checked);
    connect(diddle, &QCheckBox::checkStateChanged, this,
            [this, param](Qt::CheckState s) { set_param_diddle(param, int(s)); });
    layout->addWidget(diddle, row, 0);
    layout->addWidget(nameLabel, row, 1);

    param->widgets.clear();
    if (param->type == TypeDesc::INT) {
        auto w = new QSpinBox();
        w->setRange(-10000, 10000);
        w->setValue(param->idefault[0]);
        w->setMaximumWidth(100);
        w->setKeyboardTracking(false);
        layout->addWidget(w, row, 2);
        param->widgets.push_back(w);
        connect(w, &QSpinBox::valueChanged, this,
                [this, param](int) { set_param_instance_value(param); });
    } else if (param->type == TypeDesc::FLOAT) {
        auto w = new QtUtils::DoubleSpinBox(param->fdefault[0]);
        layout->addWidget(w, row, 2);
        param->widgets.push_back(w);
        connect(w, &QDoubleSpinBox::valueChanged, this,
                [this, param](double) { set_param_instance_value(param); });
    } else if (param->type.is_vec3()) {
        auto box = new QWidget;
        auto bl  = new QHBoxLayout;
        box->setLayout(bl);
        bl->setSpacing(1);
        for (int c = 0; c < 3; ++c) {
            std::string label = param->type == TypeColor
                                    ? std::string(1, "RGB"[c])
                                    : std::string(1, "xyz"[c]);
            bl->addWidget(QtUtils::make_qlabelfmt("{}", label));
            auto w = new QtUtils::DoubleSpinBox(param->fdefault[c]);
            if (param->type == TypeColor)
                w->setRange(0.0, 1.0);
            bl->addWidget(w);
            param->widgets.push_back(w);
            connect(w, &QDoubleSpinBox::valueChanged, this,
                    [this, param](double) { set_param_instance_value(param); });
        }
        layout->addWidget(box, row, 2);
    } else if (param->type == TypeDesc::STRING) {
        auto w = new QLineEdit();
        w->setText(param->sdefault[0].c_str());
        layout->addWidget(w, row, 2);
        param->widgets.push_back(w);
        connect(w, &QLineEdit::returnPressed, this,
                [this, param]() { set_param_instance_value(param); });
    }
    auto reset = new QPushButton("Reset");
    reset->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(reset, &QPushButton::clicked, this,
            [this, param]() { reset_param_to_default(param); });
    layout->addWidget(reset, row, 3);
    set_ui_to_paramval(param);
}



void
ToyMainWindow::set_ui_to_paramval(ParamRec* param)
{
    auto found = m_shaderparam_instvalues.find(param->name);
    const OIIO::ParamValue* pv = found != m_shaderparam_instvalues.end()
                                     ? &(*found)
                                     : nullptr;
    if (param->widgets.empty())
        return;
    if (param->type == TypeDesc::INT) {
        const int* val = pv ? (const int*)pv->data() : &param->idefault[0];
        static_cast<QSpinBox*>(param->widgets[0])->setValue(*val);
    } else if (param->type == TypeDesc::FLOAT) {
        const float* val = pv ? (const float*)pv->data() : &param->fdefault[0];
        static_cast<QDoubleSpinBox*>(param->widgets[0])->setValue(*val);
    } else if (param->type.is_vec3()) {
        const float* val = pv ? (const float*)pv->data() : &param->fdefault[0];
        for (int c = 0; c < 3; ++c)
            static_cast<QDoubleSpinBox*>(param->widgets[c])->setValue(val[c]);
    } else if (param->type == TypeDesc::STRING) {
        const ustring* val = pv ? (const ustring*)pv->data()
                                : &param->sdefault[0];
        static_cast<QLineEdit*>(param->widgets[0])->setText(val->c_str());
    }
}



void
ToyMainWindow::reset_param_to_default(ParamRec* param)
{
    m_shaderparam_instvalues.remove(param->name);
    set_ui_to_paramval(param);
    build_shader_group();
}



void
ToyMainWindow::diddle(const std::string& name)
{
    for (auto&& p : m_shaderparams)
        if (p->name == name)
            set_param_diddle(p.get(), 1);
}



void
ToyMainWindow::run_script(const std::string& script)
{
    for (auto&& cmd : OIIO::Strutil::splits(script, ";")) {
        auto w = OIIO::Strutil::splits(OIIO::Strutil::strip(cmd), " ");
        if (w.empty())
            continue;
        std::cout << "[script] " << cmd << "\n";
        if (w[0] == "recompile") {
            recompile_shaders();
            std::cout << "[script]   -> "
                      << (m_build_ok ? "ok" : "FAILED: " + m_build_error)
                      << "\n";
            continue;
        }
        ParamRec* param = nullptr;
        for (auto&& p : m_shaderparams)
            if (w.size() > 1 && p->name == w[1])
                param = p.get();
        if (!param) {
            std::cout << "[script] no param " << cmd << "\n";
            continue;
        }
        if (w[0] == "diddle" || w[0] == "undiddle") {
            set_param_diddle(param, w[0] == "diddle");
        } else if (w[0] == "set" && w.size() > 2
                   && param->type == TypeDesc::FLOAT) {
            static_cast<QDoubleSpinBox*>(param->widgets[0])
                ->setValue(OIIO::Strutil::stof(w[2]));  // -> set_param_instance_value
        }
        std::cout << "[script]   -> "
                  << (m_build_ok ? "ok" : "FAILED: " + m_build_error)
                  << " (builds so far: " << m_build_count << ")\n";
    }
}



void
ToyMainWindow::set_param_diddle(ParamRec* param, int diddle)
{
    m_diddlers[param->name.string()] = diddle;
    build_shader_group();
}



void
ToyMainWindow::set_param_instance_value(ParamRec* param)
{
    m_shaderparam_instvalues.remove(param->name);
    if (param->type == TypeDesc::INT) {
        int v = static_cast<QSpinBox*>(param->widgets[0])->value();
        m_shaderparam_instvalues.push_back(
            OIIO::ParamValue(param->name, param->type, 1, &v));
    } else if (param->type == TypeDesc::FLOAT) {
        float v = static_cast<QDoubleSpinBox*>(param->widgets[0])->value();
        m_shaderparam_instvalues.push_back(
            OIIO::ParamValue(param->name, param->type, 1, &v));
    } else if (param->type.is_vec3()) {
        float v[3];
        for (int c = 0; c < 3; ++c)
            v[c] = static_cast<QDoubleSpinBox*>(param->widgets[c])->value();
        m_shaderparam_instvalues.push_back(
            OIIO::ParamValue(param->name, param->type, 1, &v));
    } else if (param->type == TypeDesc::STRING) {
        std::string v = OIIO::Strutil::to_string(
            static_cast<QLineEdit*>(param->widgets[0])->text());
        ustring uv(v);
        m_shaderparam_instvalues.push_back(
            OIIO::ParamValue(param->name, param->type, 1, &uv));
    }

    if (m_diddlers[param->name.string()]) {
        // Interactive: just update the parameter block on the GPU.
        auto val = m_shaderparam_instvalues.find(param->name);
        if (val != m_shaderparam_instvalues.end())
            shadingsys()->ReParameter(*renderer()->shadergroup(),
                                      param->layername, param->name,
                                      param->type, val->data());
        m_view->set_params(renderer()->interactive_params());
    } else {
        build_shader_group();  // recompiles, with the value as a constant
    }
}

OSL_NAMESPACE_END
