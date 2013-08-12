/*  BOSS

    A plugin load order optimiser for games that use the esp/esm plugin system.

    Copyright (C) 2013    WrinklyNinja

    This file is part of BOSS.

    BOSS is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of
    the License, or (at your option) any later version.

    BOSS is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with BOSS.  If not, see
    <http://www.gnu.org/licenses/>.
*/
#include "main.h"
#include "settings.h"
#include "editor.h"
#include "viewer.h"

#include "../backend/globals.h"
#include "../backend/metadata.h"
#include "../backend/parsers.h"
#include "../backend/error.h"
#include "../backend/helpers.h"
#include "../backend/generators.h"
#include "../backend/network.h"
#include "../backend/streams.h"
#include "../backend/graph.h"

#include <ostream>
#include <algorithm>
#include <iterator>
#include <ctime>
#include <clocale>

#include <libloadorder.h>
#include <src/playground.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/detail/utf8_codecvt_facet.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/locale.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/thread/thread.hpp>
#include <boost/unordered_map.hpp>

#include <wx/snglinst.h>
#include <wx/aboutdlg.h>
#include <wx/progdlg.h>

#define BOOST_THREAD_VERSION 4

IMPLEMENT_APP(BossGUI);

using namespace boss;
using namespace std;
using boost::format;

namespace fs = boost::filesystem;
namespace loc = boost::locale;


struct plugin_loader {
    plugin_loader(boss::Plugin& plugin, boss::Game& game) : _plugin(plugin), _game(game) {
    }

    void operator () () {
        BOOST_LOG_TRIVIAL(info) << "Loading: " << _plugin.Name();
        _plugin = boss::Plugin(_game, _plugin.Name(), false);
        BOOST_LOG_TRIVIAL(info) << "Finished loading: " << _plugin.Name();
    }

    boss::Plugin& _plugin;
    boss::Game& _game;
    string _filename;
    bool _b;
};

struct plugin_list_loader {
    plugin_list_loader(list<boss::Plugin>& plugins, boss::Game& game) : _plugins(plugins), _game(game) {}

    void operator () () {
        for (list<boss::Plugin>::iterator it=_plugins.begin(), endit=_plugins.end(); it != endit; ++it) {
            if (skipPlugins.find(it->Name()) == skipPlugins.end()) {
                BOOST_LOG_TRIVIAL(info) << "Loading: " << it->Name();
                *it = boss::Plugin(_game, it->Name(), false);
                BOOST_LOG_TRIVIAL(info) << "Finished loading: " << it->Name();
            }
        }
    }

    list<boss::Plugin>& _plugins;
    boss::Game& _game;
    set<string> skipPlugins;
};

struct masterlist_updater_parser {
    masterlist_updater_parser(boss::Game& game, list<boss::Message>& errors, list<boss::Plugin>& plugins, list<boss::Message>& messages, string& revision) : _game(game), _errors(errors), _plugins(plugins), _messages(messages), _revision(revision) {}

    void operator () () {

        BOOST_LOG_TRIVIAL(trace) << "Updating masterlist";
        try {
            _revision = UpdateMasterlist(_game, _errors, _plugins, _messages);
        } catch (boss::error& e) {
            _plugins.clear();
            _messages.clear();
            BOOST_LOG_TRIVIAL(error) << "Masterlist update failed. Details: " << e.what();
            _errors.push_back(boss::Message(boss::g_message_error, (format(loc::translate("Masterlist update failed. Details: %1%")) % e.what()).str()));
        }

        if (_plugins.empty() && _messages.empty() && fs::exists(_game.MasterlistPath())) {

            BOOST_LOG_TRIVIAL(trace) << "Parsing masterlist...";
            try {
                YAML::Node mlist = YAML::LoadFile(_game.MasterlistPath().string());

                if (mlist["globals"])
                    _messages = mlist["globals"].as< list<boss::Message> >();
                if (mlist["plugins"])
                    _plugins = mlist["plugins"].as< list<boss::Plugin> >();
            } catch (YAML::Exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Masterlist parsing failed. Details: " << e.what();
                _errors.push_back(boss::Message(boss::g_message_error, (format(loc::translate("Masterlist parsing failed. Details: %1%")) % e.what()).str()));
            }
            BOOST_LOG_TRIVIAL(trace) << "Finished parsing masterlist.";
        }
    }

    boss::Game& _game;
    list<boss::Message>& _errors;
    list<boss::Plugin>& _plugins;
    list<boss::Message>& _messages;
    string& _revision;
};

bool BossGUI::OnInit() {

    //Check if GUI is already running.
	wxSingleInstanceChecker *checker = new wxSingleInstanceChecker;

	if (checker->IsAnotherRunning()) {
        wxMessageBox(
			translate("Error: BOSS is already running. This instance will now quit."),
			translate("BOSS: Error"),
			wxOK | wxICON_ERROR,
			NULL);
		delete checker; // OnExit() won't be called if we return false
		checker = NULL;

		return false;
	}

    //Load settings.
    if (!fs::exists(g_path_settings)) {
        try {
            if (!fs::exists(g_path_settings.parent_path()))
                fs::create_directory(g_path_settings.parent_path());
        } catch (fs::filesystem_error& e) {
            wxMessageBox(
				translate("Error: Could not create local app data BOSS folder."),
				translate("BOSS: Error"),
				wxOK | wxICON_ERROR,
				NULL);
            return false;
        }
        GenerateDefaultSettingsFile(g_path_settings.string());
    }
    if (fs::exists(g_path_settings)) {
        try {
            _settings = YAML::LoadFile(g_path_settings.string());
        } catch (YAML::ParserException& e) {
            //LOG_ERROR("Error: %s", e.getString().c_str());
            wxMessageBox(
				FromUTF8(format(loc::translate("Error: Settings parsing failed. %1%")) % e.what()),
				translate("BOSS: Error"),
				wxOK | wxICON_ERROR,
				NULL);
            return false;
        }
    }

    //Set up logging.
    unsigned int verbosity = _settings["Debug Verbosity"].as<unsigned int>();
    if (verbosity > 0) {
        boost::log::add_file_log(
            boost::log::keywords::file_name = g_path_log.string().c_str(),
            boost::log::keywords::format = (
                boost::log::expressions::stream
                    << "[" << boost::log::expressions::format_date_time< boost::posix_time::ptime >("TimeStamp", "%H:%M:%S") << "]"
                    << " [" << boost::log::trivial::severity << "]: "
                    << boost::log::expressions::smessage
                )
            );
        if (verbosity == 1)
            boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::warning);  //Log all warnings, errors and fatals.
        else if (verbosity == 2)
            boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::debug);  //Log debugs, infos, warnings, errors and fatals.
        else {
            boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::trace);  //Log everything.
        }
        boost::log::add_common_attributes();
    } else
        boost::log::core::get()->set_logging_enabled(false);  //Disable all logging.

    //Specify location of language dictionaries
	boost::locale::generator gen;
	gen.add_messages_path(g_path_l10n.string());
	gen.add_messages_domain("messages");

    //Set the locale to get encoding and language conversions working correctly.
    BOOST_LOG_TRIVIAL(debug) << "Initialising language settings.";
    if (_settings["Language"]) {
        //Defaults in case language string is empty.
        string localeId = "en.UTF-8";
        wxLanguage lang = wxLANGUAGE_ENGLISH;
        if (_settings["Language"].as<string>() == "eng") {
            localeId = "en.UTF-8";
            lang = wxLANGUAGE_ENGLISH;
            BOOST_LOG_TRIVIAL(debug) << "Setting language to English.";
        } else if (_settings["Language"].as<string>() == "spa") {
            localeId = "es.UTF-8";
            lang = wxLANGUAGE_SPANISH;
            BOOST_LOG_TRIVIAL(debug) << "Setting language to Spanish.";
        } else if (_settings["Language"].as<string>() == "rus") {
            localeId = "ru.UTF-8";
            lang = wxLANGUAGE_RUSSIAN;
            BOOST_LOG_TRIVIAL(debug) << "Setting language to Russian.";
        }

        try {
            locale::global(gen(localeId));
            cout.imbue(locale());
            //Need to also set up wxWidgets locale so that its default interface text comes out in the right language.
            wxLoc = new wxLocale();
            if (!wxLoc->Init(lang, wxLOCALE_LOAD_DEFAULT))
                throw runtime_error("System GUI text could not be set.");
            wxLocale::AddCatalogLookupPathPrefix(g_path_l10n.string().c_str());
            wxLoc->AddCatalog("wxstd");
        } catch(runtime_error &e) {
            BOOST_LOG_TRIVIAL(error) << "Could not implement translation: " << e.what();
            wxMessageBox(
                FromUTF8(format(loc::translate("Error: could not apply translation: %1%")) % e.what()),
                translate("BOSS: Error"),
                wxOK | wxICON_ERROR,
                NULL);
        }
        locale global_loc = locale();
        locale loc(global_loc, new boost::filesystem::detail::utf8_codecvt_facet());
        boost::filesystem::path::imbue(loc);
    }

    //Detect installed games.
    BOOST_LOG_TRIVIAL(debug) << "Detecting installed games.";
    try {
        _games = GetGames(_settings);
    } catch (boss::error& e) {
        BOOST_LOG_TRIVIAL(error) << "Game-specific settings could not be initialised. " << e.what();
        wxMessageBox(
            FromUTF8(format(loc::translate("Error: Game-specific settings could not be initialised. %1%")) % e.what()),
            translate("BOSS: Error"),
            wxOK | wxICON_ERROR,
            NULL);
        return false;
    } catch (YAML::Exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Games' settings parsing failed. " << e.what();
        wxMessageBox(
            FromUTF8(format(loc::translate("Error: Games' settings parsing failed. %1%")) % e.what()),
            translate("BOSS: Error"),
            wxOK | wxICON_ERROR,
            NULL);
        return false;
    }

    BOOST_LOG_TRIVIAL(debug) << "Selecting game.";
    string target;
    if (_settings["Game"] && _settings["Game"].as<string>() != "auto")
        target = _settings["Game"].as<string>();
    else if (_settings["Last Game"] && _settings["Last Game"].as<string>() != "auto")
        target = _settings["Last Game"].as<string>();

    if (!target.empty()) {
        for (size_t i=0, max=_games.size(); i < max; ++i) {
            if (target == _games[i].FolderName() && _games[i].IsInstalled())
                _game = _games[i];
        }
    }
    if (_game == Game()) {
        //Set _game to the first installed game.
        for (size_t i=0, max=_games.size(); i < max; ++i) {
            if (_games[i].IsInstalled()) {
                _game = _games[i];
                break;
            }
        }
        if (_game == Game()) {
            BOOST_LOG_TRIVIAL(error) << "None of the supported games were detected.";
            wxMessageBox(
                translate("Error: None of the supported games were detected."),
                translate("BOSS: Error"),
                wxOK | wxICON_ERROR,
                NULL);
            return false;
        }
    }

    //Now that game is selected, initialise it.
    BOOST_LOG_TRIVIAL(debug) << "Initialising game-specific settings.";
    try {
        _game.Init();
        *find(_games.begin(), _games.end(), _game) = _game;  //Sync changes.
    } catch (boss::error& e) {
        BOOST_LOG_TRIVIAL(error) << "Game-specific settings could not be initialised. " << e.what();
        wxMessageBox(
            FromUTF8(format(loc::translate("Error: Game-specific settings could not be initialised. %1%")) % e.what()),
            translate("BOSS: Error"),
            wxOK | wxICON_ERROR,
            NULL);
        return false;
    }

    //Create launcher window.
    BOOST_LOG_TRIVIAL(debug) << "Opening the main BOSS window.";
    Launcher * launcher = new Launcher(wxT("BOSS"), _settings, _game, _games);

    launcher->SetIcon(wxIconLocation("BOSS.exe"));
	launcher->Show();
	SetTopWindow(launcher);

    return true;
}

Launcher::Launcher(const wxChar *title, YAML::Node& settings, Game& game, vector<boss::Game>& games) : wxFrame(NULL, wxID_ANY, title), _game(game), _settings(settings), _games(games) {

    //Initialise menu items.
    wxMenuBar * MenuBar = new wxMenuBar();
    wxMenu * FileMenu = new wxMenu();
    wxMenu * EditMenu = new wxMenu();
    GameMenu = new wxMenu();
    wxMenu * HelpMenu = new wxMenu();

	//Initialise controls.
    wxButton * EditButton = new wxButton(this, OPTION_EditMetadata, translate("Edit Metadata"));
    wxButton * SortButton = new wxButton(this, OPTION_SortPlugins, translate("Sort Plugins"));
    ViewButton = new wxButton(this,OPTION_ViewLastReport, translate("View Last Report"));

    //Construct menus.
    //File Menu
	FileMenu->Append(OPTION_ViewLastReport, translate("&View Last Report"));
    FileMenu->Append(OPTION_SortPlugins, translate("&Sort Plugins"));
    FileMenu->AppendSeparator();
    FileMenu->Append(wxID_EXIT);
    MenuBar->Append(FileMenu, translate("&File"));
	//Edit Menu
	EditMenu->Append(OPTION_EditMetadata, translate("&Metadata..."));
	EditMenu->Append(MENU_ShowSettings, translate("&Settings..."));
	MenuBar->Append(EditMenu, translate("&Edit"));
	//Game menu - set up initial item states here too.
    for (size_t i=0,max=_games.size(); i < max; ++i) {
        wxMenuItem * item = GameMenu->AppendRadioItem(MENU_LowestDynamicGameID + i, FromUTF8(_games[i].Name()));
        if (_game == _games[i])
            item->Check();

        if (_games[i].IsInstalled())
            Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnGameChange, this, MENU_LowestDynamicGameID + i);
        else
            item->Enable(false);
    }
	MenuBar->Append(GameMenu, translate("&Game"));
    //About menu
	HelpMenu->Append(wxID_HELP);
	HelpMenu->AppendSeparator();
	HelpMenu->Append(wxID_ABOUT);
    MenuBar->Append(HelpMenu, translate("&Help"));

    //Set up layout.
    wxBoxSizer *buttonBox = new wxBoxSizer(wxVERTICAL);
	buttonBox->Add(EditButton, 1, wxEXPAND|wxALIGN_CENTRE|wxALL, 10);
	buttonBox->Add(SortButton, 1, wxEXPAND|wxALIGN_CENTRE|wxLEFT|wxRIGHT, 10);
	buttonBox->Add(ViewButton, 1, wxEXPAND|wxALIGN_CENTRE|wxALL, 10);

    //Bind event handlers.
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnQuit, this, wxID_EXIT);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnViewLastReport, this, OPTION_ViewLastReport);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnSortPlugins, this, OPTION_SortPlugins);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnEditMetadata, this, OPTION_EditMetadata);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnOpenSettings, this, MENU_ShowSettings);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnHelp, this, wxID_HELP);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &Launcher::OnAbout, this, wxID_ABOUT);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &Launcher::OnSortPlugins, this, OPTION_SortPlugins);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &Launcher::OnEditMetadata, this, OPTION_EditMetadata);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &Launcher::OnViewLastReport, this, OPTION_ViewLastReport);
    Bind(wxEVT_CLOSE_WINDOW, &Launcher::OnClose, this);

    //Set up initial state.
    SortButton->SetDefault();

    if (!fs::exists(_game.ReportPath()))
        ViewButton->Enable(false);

    //Set title bar text.
	SetTitle(FromUTF8("BOSS - " + _game.Name()));

    //Now set the layout and sizes.
    SetMenuBar(MenuBar);
	SetBackgroundColour(wxColour(255,255,255));
	SetSizerAndFit(buttonBox);
    SetSize(wxSize(250, 200));
}

//Called when the frame exits.
void Launcher::OnQuit(wxCommandEvent& event) {

	Close(true); // Tells the OS to quit running this process
}

void Launcher::OnClose(wxCloseEvent& event) {
    BOOST_LOG_TRIVIAL(debug) << "Quiting BOSS.";

    _settings["Last Game"] = _game.FolderName();

    _settings["Games"] = _games;

    //Save settings.
    BOOST_LOG_TRIVIAL(debug) << "Saving BOSS settings.";
    YAML::Emitter yout;
    yout.SetIndent(2);
    yout << _settings;

    boss::ofstream out(boss::g_path_settings);
    out << yout.c_str();
    out.close();

    Destroy();
}

void Launcher::OnSortPlugins(wxCommandEvent& event) {

    BOOST_LOG_TRIVIAL(debug) << "Beginning sorting process.";

    YAML::Node mlist, ulist;
    list<boss::Message> messages, mlist_messages, ulist_messages;
    list<boss::Plugin> mlist_plugins, ulist_plugins;
    boost::thread_group group;
    list<boss::Plugin> plugins;
    string revision;

    wxProgressDialog *progDia = new wxProgressDialog(translate("BOSS: Working..."),translate("BOSS working..."), 1000, this, wxPD_APP_MODAL|wxPD_AUTO_HIDE|wxPD_ELAPSED_TIME);

    masterlist_updater_parser mup(_game, messages, mlist_plugins, mlist_messages, revision);
    group.create_thread(mup);

    //First calculate the mean plugin size. Store it temporarily in a map to reduce filesystem lookups and file size recalculation.
    size_t meanFileSize = 0;
    boost::unordered_map<size_t, boss::Plugin> tempMap;
    for (fs::directory_iterator it(_game.DataPath()); it != fs::directory_iterator(); ++it) {
        if (fs::is_regular_file(it->status()) && IsPlugin(it->path().string())) {
            size_t fileSize = fs::file_size(it->path());
            meanFileSize += fileSize;

            tempMap.emplace(fileSize, boss::Plugin(it->path().filename().string()));
        }
    }
    meanFileSize /= tempMap.size();

    //Now load plugins.
    plugin_list_loader pll(plugins, _game);
    for (boost::unordered_map<size_t, boss::Plugin>::const_iterator it=tempMap.begin(), endit=tempMap.end(); it != endit; ++it) {

        BOOST_LOG_TRIVIAL(trace) << "Found plugin: " << it->second.Name();

        plugins.push_back(it->second);

        if (it->first > meanFileSize) {
            plugin_loader pl(plugins.back(), _game);
            pll.skipPlugins.insert(it->second.Name());
            group.create_thread(pl);
        }

        progDia->Pulse();
    }
    group.create_thread(pll);
    group.join_all();

    //Now load userlist.
    if (fs::exists(_game.UserlistPath())) {
        BOOST_LOG_TRIVIAL(trace) << "Parsing userlist...";

        try {
            YAML::Node ulist = YAML::LoadFile(_game.UserlistPath().string());

            if (ulist["plugins"])
                ulist_plugins = ulist["plugins"].as< list<boss::Plugin> >();
        } catch (YAML::ParserException& e) {
            BOOST_LOG_TRIVIAL(error) << "Userlist parsing failed. Details: " << e.what();
            messages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("Userlist parsing failed. Details: %1%")) % e.what()).str()));
        }
        if (ulist["plugins"])
            ulist_plugins = ulist["plugins"].as< list<boss::Plugin> >();
    }

    progDia->Pulse();

    if (fs::exists(_game.MasterlistPath()) || fs::exists(_game.UserlistPath())) {
        BOOST_LOG_TRIVIAL(trace) << "Merging plugin lists, evaluating conditions and checking for install validity...";

         //Set language.
        unsigned int lang = GetLangNum(_settings["Language"].as<string>());


        //Merge all global message lists.
        BOOST_LOG_TRIVIAL(trace) << "Merging all global message lists.";
        if (!mlist_messages.empty())
            messages.insert(messages.end(), mlist_messages.begin(), mlist_messages.end());
        if (!ulist_messages.empty())
            messages.insert(messages.end(), ulist_messages.begin(), ulist_messages.end());

        //Evaluate any conditions in the global messages.
        BOOST_LOG_TRIVIAL(trace) << "Evaluating global message conditions...";
        try {
            list<boss::Message>::iterator it=messages.begin();
            while (it != messages.end()) {
                if (!it->EvalCondition(_game, lang))
                    it = messages.erase(it);
                else
                    ++it;
            }
        } catch (boss::error& e) {
            BOOST_LOG_TRIVIAL(error) << "A global message contains a condition that could not be evaluated. Details: " << e.what();
            messages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("A global message contains a condition that could not be evaluated. Details: %1%")) % e.what()).str()));
        }

        //Merge plugin list, masterlist and userlist plugin data.
        BOOST_LOG_TRIVIAL(trace) << "Merging plugin list, masterlist and userlist data.";
        map<string, bool> consistencyIssues;
        for (list<boss::Plugin>::iterator it=plugins.begin(), endIt=plugins.end(); it != endIt; ++it) {
            BOOST_LOG_TRIVIAL(trace) << "Merging for plugin \"" << it->Name() << "\"";
            //Check if there is already a plugin in the 'plugins' list or not.
            list<boss::Plugin>::iterator pos = std::find(mlist_plugins.begin(), mlist_plugins.end(), *it);

            if (pos != mlist_plugins.end()) {
                BOOST_LOG_TRIVIAL(trace) << "Merging masterlist data down to plugin list data.";
                it->Merge(*pos);
            }

            pos = std::find(ulist_plugins.begin(), ulist_plugins.end(), *it);

            if (pos != ulist_plugins.end()) {
                BOOST_LOG_TRIVIAL(trace) << "Merging userlist data down to plugin list data.";
                it->Merge(*pos);
            }

            progDia->Pulse();

            //Now that items are merged, evaluate any conditions they have.
            BOOST_LOG_TRIVIAL(trace) << "Evaluate conditions for merged plugin data.";
            try {
                it->EvalAllConditions(_game, lang);
            } catch (boss::error& e) {
                BOOST_LOG_TRIVIAL(error) << "\"" << it->Name() << "\" contains a condition that could not be evaluated. Details: " << e.what();
                messages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("\"%1%\" contains a condition that could not be evaluated. Details: %2%")) % it->Name() % e.what()).str()));
            }

            progDia->Pulse();

            //Also check install validity.
            BOOST_LOG_TRIVIAL(trace) << "Checking that the current install is valid according to this plugin's data.";
            map<string, bool> issues = it->CheckInstallValidity(_game);
            list<boss::Message> pluginMessages = it->Messages();
            for (map<string,bool>::const_iterator jt=issues.begin(), endJt=issues.end(); jt != endJt; ++jt) {
                if (jt->second) {
                    BOOST_LOG_TRIVIAL(error) << "\"" << jt->first << "\" is incompatible with \"" << it->Name() << "\" and is present.";
                    pluginMessages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("\"%1%\" is incompatible with \"%2%\" and is present.")) % jt->first % it->Name()).str()));
                } else {
                    BOOST_LOG_TRIVIAL(error) << "\"" << jt->first << "\" is required by \"" << it->Name() << "\" but is missing.";
                    pluginMessages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("\"%1%\" is required by \"%2%\" but is missing.")) % jt->first % it->Name()).str()));
                }
            }
            if (!issues.empty())
                it->Messages(pluginMessages);

            progDia->Pulse();
        }
    }

    progDia->Pulse();

    //First sort the plugins list so that masters come before non-masters.
    BOOST_LOG_TRIVIAL(trace) << "Moving masters before non-masters.";
    plugins.sort(boss::master_sort);

    progDia->Pulse();

    //Now build overlap map.
    BOOST_LOG_TRIVIAL(trace) << "Building plugin overlap map.";
    boost::unordered_map< string, vector<string> > overlapMap;
    CalcPluginOverlaps(plugins, overlapMap);

    progDia->Pulse();

    BOOST_LOG_TRIVIAL(trace) << "Building the plugin dependency graph...";

    //Use an adjacency list (don't know yet if list or matrix is the better choice), and use "listS" as the VertexList type. We need a possible multi-graph to catch some forms of cyclic dependency (a working graph would not be a multi-graph though), so use "listS". Want a directed graph where we can access in-edges, so use "bidirectionalS". Also provide the boss::Plugin class as the vertex property type.

    boss::PluginGraph graph;

    //Now add the plugins in order to the graph as vertices.
    for (list<boss::Plugin>::iterator it=plugins.begin(), endIt=plugins.end(); it != endIt; ++it) {
        BOOST_LOG_TRIVIAL(trace) << "Creating vertex for \"" << it->Name() << "\".";
        boost::add_vertex(it, graph);
    }

    //Vertices are numbered from 0 to (N - 1), where N is the number of vertices in the graph. A plugin's vertex number is therefore the same as its position in the list, and this can be used to create edges between plugins.


    boss::vertex_it vitFirstNonMaster, vit, vitend;
    for (boost::tie(vit, vitend) = boost::vertices(graph); vit != vitend; ++vit) {
        if (!graph[*vit]->IsMaster()) {
            vitFirstNonMaster = vit;
            break;
        }
    }

    for (boost::tie(vit, vitend) = boost::vertices(graph); vit != vitend; ++vit) {
        vertex_t parentVertex;

        BOOST_LOG_TRIVIAL(trace) << "Adding edges to vertex for \"" << graph[*vit]->Name() << "\".";

        if (graph[*vit]->IsMaster()) {
            BOOST_LOG_TRIVIAL(trace) << "Adding out-edges for non-master plugins.";

            for (boss::vertex_it vit2 = vitFirstNonMaster; vit2 != vitend; ++vit2) {
                BOOST_LOG_TRIVIAL(trace) << "Adding edge from \"" << graph[*vit]->Name() << "\" to \"" << graph[*vit2]->Name() << "\".";

                if (!boost::edge(*vit, *vit2, graph).second)  //To avoid duplicates (helps visualisation).
                    boost::add_edge(*vit, *vit2, graph);
            }
        }

        BOOST_LOG_TRIVIAL(trace) << "Adding in-edges for masters.";
        vector<string> strVec(graph[*vit]->Masters());
        for (vector<string>::const_iterator it=strVec.begin(), itend=strVec.end(); it != itend; ++it) {
            if (boss::GetVertexByName(graph, *it, parentVertex) &&
                !boost::edge(parentVertex, *vit, graph).second) {

                BOOST_LOG_TRIVIAL(trace) << "Adding edge from \"" << graph[parentVertex]->Name() << "\" to \"" << graph[*vit]->Name() << "\".";

                boost::add_edge(parentVertex, *vit, graph);
            }
        }
        BOOST_LOG_TRIVIAL(trace) << "Adding in-edges for requirements.";
        set<File> fileset(graph[*vit]->Reqs());
        for (set<File>::const_iterator it=fileset.begin(), itend=fileset.end(); it != itend; ++it) {
            if (boss::IsPlugin(it->Name()) &&
                boss::GetVertexByName(graph, it->Name(), parentVertex) &&
                !boost::edge(parentVertex, *vit, graph).second) {

                BOOST_LOG_TRIVIAL(trace) << "Adding edge from \"" << graph[parentVertex]->Name() << "\" to \"" << graph[*vit]->Name() << "\".";

                boost::add_edge(parentVertex, *vit, graph);
            }
        }

        BOOST_LOG_TRIVIAL(trace) << "Adding in-edges for 'load after's.";
        fileset = graph[*vit]->LoadAfter();
        for (set<File>::const_iterator it=fileset.begin(), itend=fileset.end(); it != itend; ++it) {
            if (boss::IsPlugin(it->Name()) &&
                boss::GetVertexByName(graph, it->Name(), parentVertex) &&
                !boost::edge(parentVertex, *vit, graph).second) {

                BOOST_LOG_TRIVIAL(trace) << "Adding edge from \"" << graph[parentVertex]->Name() << "\" to \"" << graph[*vit]->Name() << "\".";

                boost::add_edge(parentVertex, *vit, graph);
            }
        }


        //Now add any overlaps, except where an edge already exists between the two plugins, going the other way, since overlap-based edges have the lowest priority and are not a candidate for causing cyclic loop errors.
        BOOST_LOG_TRIVIAL(trace) << "Adding in-edges for overlaps.";
        boost::unordered_map< string, vector<string> >::const_iterator overlapIt = overlapMap.find(graph[*vit]->Name());
        if (overlapIt != overlapMap.end()) {
            for (vector<string>::const_iterator it=overlapIt->second.begin(), itend=overlapIt->second.end(); it != itend; ++it) {
                if (boss::GetVertexByName(graph, *it, parentVertex) &&
                    !boost::edge(*vit, parentVertex, graph).second) {  //No edge going the other way, OK to add this edge.

                    BOOST_LOG_TRIVIAL(trace) << "Adding edge from \"" << graph[parentVertex]->Name() << "\" to \"" << graph[*vit]->Name() << "\".";

                    boost::add_edge(parentVertex, *vit, graph);
                }
            }
        }

        progDia->Pulse();

    }

    //Just for fun - output the graph as a ".dot" file for external rendering. If I can get this pretty, I might add it to a tab in the BOSS Report - here's a few Javascript libraries for displaying .dot files, but in my test case the graph is too complex and both libraries I found ran out of memory.
    //The .dot file can be converted to an SVG using Graphviz: the command is `dot -Tsvg output.dot -o output.svg`.
    if (fs::exists(g_path_graphvis)) {
        BOOST_LOG_TRIVIAL(trace) << "Outputting the graph.";
        fs::path temp = fs::path(_game.GraphPath().string() + ".temp");
        boss::SaveGraph(graph, temp);

        string command = g_path_graphvis.string() + " -Tsvg \"" + temp.string() + "\" -o \"" + _game.GraphPath().string() + "\"";
        string output;

        try {
            system(command.c_str());

  //          if (RunCommand(command, output))  //This hangs for graphvis, for some reason. Works for svn though...
            fs::remove(temp);
        } catch(boss::error& e) {
            messages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("Failed to generate graph image. Details: %1%")) % e.what()).str()));
        }
    }

    //Check for back-edges, then perform a topological sort.
    try {
        BOOST_LOG_TRIVIAL(trace) << "Checking to see if the graph is cyclic.";
        boss::CheckForCycles(graph);

        progDia->Pulse();

        BOOST_LOG_TRIVIAL(trace) << "Performing a topological sort.";
        boss::Sort(graph, plugins);

        progDia->Pulse();

        BOOST_LOG_TRIVIAL(debug) << "Displaying load order preview.";
        LoadOrderPreview preview(this, translate("BOSS: Calculated Load Order"), plugins);

        if (preview.ShowModal() == wxID_OK) {
            BOOST_LOG_TRIVIAL(trace) << "Load order accepted.";
            list<boss::Plugin> newPluginsList, editedPlugins;
            newPluginsList = preview.GetLoadOrder();

            BOOST_LOG_TRIVIAL(trace) << "Building list of edited plugins.";
            for (list<boss::Plugin>::iterator it=newPluginsList.begin(),endit=newPluginsList.end(); it != endit; ++it) {
                list<boss::Plugin>::const_iterator jt = find(plugins.begin(), plugins.end(), *it);
                boss::Plugin diff = it->DiffMetadata(*jt);

                if (!diff.HasNameOnly())
                    editedPlugins.push_back(diff);
            }
            plugins = newPluginsList;

            BOOST_LOG_TRIVIAL(trace) << "Merging down edits to the userlist.";
            for (list<boss::Plugin>::const_iterator it=editedPlugins.begin(),endit=editedPlugins.end(); it != endit; ++it) {
                list<boss::Plugin>::iterator jt = find(ulist_plugins.begin(), ulist_plugins.end(), *it);

                if (jt != ulist_plugins.end()) {
                    jt->Merge(*it);
                } else {
                    ulist_plugins.push_back(*it);
                }
            }

            BOOST_LOG_TRIVIAL(trace) << "Checking that any pre-existing 'load after' entries in the userlist are still valid.";
            for (list<boss::Plugin>::iterator it=ulist_plugins.begin(),endit=ulist_plugins.end(); it != endit; ++it) {
                //Double-check that the plugin is still loading after any other plugins already in its "load after" metadata.
                list<boss::Plugin>::iterator mainPos = find(plugins.begin(), plugins.end(), *it);

                if (mainPos == plugins.end())
                    continue;

                size_t mainDist = distance(plugins.begin(), mainPos);

                set<boss::File> loadAfter = it->LoadAfter();
                set<boss::File>::iterator jt = loadAfter.begin();
                while (jt != loadAfter.end()) {
                    list<boss::Plugin>::iterator filePos = find(plugins.begin(), plugins.end(), *jt);

                    size_t fileDist = distance(plugins.begin(), filePos);

                    if (fileDist > mainDist)
                        loadAfter.erase(jt++);
                    else
                        ++jt;
                }
                it->LoadAfter(loadAfter);
            }

            //Save edits to userlist.
            BOOST_LOG_TRIVIAL(debug) << "Saving edited userlist.";
            YAML::Emitter yout;
            yout.SetIndent(2);
            yout << YAML::BeginMap
                 << YAML::Key << "plugins" << YAML::Value << ulist_plugins
                 << YAML::EndMap;

            boss::ofstream uout(_game.UserlistPath());
            uout << yout.c_str();
            uout.close();

            //Now set load order.
            BOOST_LOG_TRIVIAL(trace) << "Setting load order.";
            try {
                _game.SetLoadOrder(plugins);
            } catch (boss::error& e) {
                BOOST_LOG_TRIVIAL(error) << "Failed to set the load order. Details: " << e.what();
                messages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("Failed to set the load order. Details: %1%")) % e.what()).str()));
            }
        } else {
            BOOST_LOG_TRIVIAL(debug) << "The load order calculated was not applied as sorting was canceled.";
            messages.push_back(boss::Message(boss::g_message_warn, loc::translate("The load order displayed in the Details tab was not applied as sorting was canceled.")));
        }

        BOOST_LOG_TRIVIAL(trace) << "Load order set:";
        for (list<boss::Plugin>::iterator it=plugins.begin(), endIt = plugins.end(); it != endIt; ++it) {
            BOOST_LOG_TRIVIAL(trace) << '\t' << it->Name();
        }
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to calculate the load order. Details: " << e.what();
         messages.push_back(boss::Message(boss::g_message_error, (format(loc::translate("Failed to calculate the load order. Details: %1%")) % e.what()).str()));
    }

    //Read the details section of the previous report, if it exists.
    string oldDetails;
    if (fs::exists(_game.ReportPath().string())) {
        BOOST_LOG_TRIVIAL(debug) << "Reading the previous report's details section.";
        //Read the whole file in.
        boss::ifstream in(_game.ReportPath(), ios::binary);
        in.seekg(0, std::ios::end);
        oldDetails.resize(in.tellg());
        in.seekg(0, std::ios::beg);
        in.read(&oldDetails[0], oldDetails.size());
        in.close();

        //Slim down to only the details section.
        size_t pos1 = oldDetails.find("<div id=\"plugins\"");
        pos1 = oldDetails.find("<ul>", pos1);
        size_t pos2 = oldDetails.find("</div>", pos1);
        pos2 = oldDetails.rfind("</ul>", pos2) - 3;  //Remove the 3 tabs preceding the closing tag.

        oldDetails = oldDetails.substr(pos1, pos2 - pos1);
        boost::replace_all(oldDetails, "\t\t\t\t", "\t");
        oldDetails += "</ul>\n";
    }

    BOOST_LOG_TRIVIAL(debug) << "Generating report...";
    try {
        GenerateReport(_game.ReportPath().string(),
                        messages,
                        plugins,
                        oldDetails,
                        revision,
                        _settings["Update Masterlist"].as<bool>());
    } catch (boss::error& e) {
        wxMessageBox(
            FromUTF8(format(loc::translate("Error: %1%")) % e.what()),
            translate("BOSS: Error"),
            wxOK | wxICON_ERROR,
            this);
        progDia->Destroy();
        return;
    }

    progDia->Pulse();

    //Now a results report definitely exists.
    ViewButton->Enable(true);

    progDia->Destroy();

    BOOST_LOG_TRIVIAL(debug) << "Displaying report...";
    if (_settings["View Report Externally"] && _settings["View Report Externally"].as<bool>()) {
        wxLaunchDefaultApplication(_game.ReportPath().string());
    } else {
        //Create viewer window.
        Viewer *viewer = new Viewer(this, translate("BOSS: Report Viewer"), _game.ReportPath().string());
        viewer->Show();
    }

    BOOST_LOG_TRIVIAL(debug) << "Report display successful. Sorting process complete.";
}

void Launcher::OnEditMetadata(wxCommandEvent& event) {

    //Should probably check for masterlist updates before opening metadata editor.
    vector<boss::Plugin> installed, mlist_plugins, ulist_plugins;

    wxProgressDialog *progDia = new wxProgressDialog(translate("BOSS: Working..."),translate("BOSS working..."), 1000, this, wxPD_APP_MODAL|wxPD_AUTO_HIDE|wxPD_ELAPSED_TIME);

    //Scan for installed plugins.
    BOOST_LOG_TRIVIAL(debug) << "Reading installed plugins' headers.";
    for (fs::directory_iterator it(_game.DataPath()); it != fs::directory_iterator(); ++it) {
        if (fs::is_regular_file(it->status()) && IsPlugin(it->path().string())) {
			boss::Plugin plugin(_game, it->path().filename().string(), true);
            installed.push_back(plugin);

            progDia->Pulse();
        }
    }

    //Parse masterlist.
    if (fs::exists(_game.MasterlistPath())) {
        BOOST_LOG_TRIVIAL(debug) << "Parsing masterlist.";
        YAML::Node mlist;
        try {
            mlist = YAML::LoadFile(_game.MasterlistPath().string());
        } catch (YAML::ParserException& e) {
            BOOST_LOG_TRIVIAL(error) << "Masterlist parsing failed. " << e.what();
            wxMessageBox(
                FromUTF8(format(loc::translate("Error: Masterlist parsing failed. %1%")) % e.what()),
                translate("BOSS: Error"),
                wxOK | wxICON_ERROR,
                this);
        }
        if (mlist["plugins"])
            mlist_plugins = mlist["plugins"].as< vector<boss::Plugin> >();
    }

    progDia->Pulse();

    //Parse userlist.
    if (fs::exists(_game.UserlistPath())) {
        BOOST_LOG_TRIVIAL(debug) << "Parsing userlist.";
        YAML::Node ulist;
        try {
            ulist = YAML::LoadFile(_game.UserlistPath().string());
        } catch (YAML::ParserException& e) {
            BOOST_LOG_TRIVIAL(error) << "Userlist parsing failed. " << e.what();
            wxMessageBox(
				FromUTF8(format(loc::translate("Error: Userlist parsing failed. %1%")) % e.what()),
				translate("BOSS: Error"),
				wxOK | wxICON_ERROR,
				this);
        }
        if (ulist["plugins"])
            ulist_plugins = ulist["plugins"].as< vector<boss::Plugin> >();
    }

    progDia->Pulse();

    //Merge the masterlist down into the installed mods list.
    BOOST_LOG_TRIVIAL(debug) << "Merging the masterlist down into the installed mods list.";
    for (vector<boss::Plugin>::const_iterator it=mlist_plugins.begin(), endit=mlist_plugins.end(); it != endit; ++it) {
        vector<boss::Plugin>::iterator pos = find(installed.begin(), installed.end(), *it);

        if (pos != installed.end())
            pos->Merge(*it);
    }


    progDia->Pulse();

    //Add empty entries for any userlist entries that aren't installed.
    BOOST_LOG_TRIVIAL(debug) << "Padding the userlist to match the plugins in the installed mods list.";
    for (vector<boss::Plugin>::const_iterator it=ulist_plugins.begin(), endit=ulist_plugins.end(); it != endit; ++it) {
        if (find(installed.begin(), installed.end(), *it) == installed.end())
            installed.push_back(boss::Plugin(it->Name()));
    }

    progDia->Pulse();

    //Sort into alphabetical order.
    BOOST_LOG_TRIVIAL(debug) << "Sorting plugin list into alphabetical order.";
    std::sort(installed.begin(), installed.end(), boss::alpha_sort);

    progDia->Pulse();

    unsigned int lang = GetLangNum(_settings["Language"].as<string>());

    //Create editor window.
    BOOST_LOG_TRIVIAL(debug) << "Opening editor window.";
    Editor *editor = new Editor(this, translate("BOSS: Metadata Editor"), _game.UserlistPath().string(), installed, ulist_plugins, lang);

    progDia->Destroy();

	editor->Show();
    BOOST_LOG_TRIVIAL(debug) << "Editor window opened.";
}

void Launcher::OnViewLastReport(wxCommandEvent& event) {
    if (_settings["View Report Externally"] && _settings["View Report Externally"].as<bool>()) {
        BOOST_LOG_TRIVIAL(debug) << "Opening report in external application...";
        wxLaunchDefaultApplication(_game.ReportPath().string());
    } else {
        //Create viewer window.
        BOOST_LOG_TRIVIAL(debug) << "Opening viewer window...";
        Viewer *viewer = new Viewer(this, translate("BOSS: Report Viewer"), _game.ReportPath().string());
        viewer->Show();
    }
    BOOST_LOG_TRIVIAL(debug) << "Report displayed.";
}

void Launcher::OnOpenSettings(wxCommandEvent& event) {
    BOOST_LOG_TRIVIAL(debug) << "Opening settings window...";
	SettingsFrame *settings = new SettingsFrame(this, translate("BOSS: Settings"), _settings, _games);
	settings->ShowModal();
    BOOST_LOG_TRIVIAL(debug) << "Editor window opened.";
}

void Launcher::OnGameChange(wxCommandEvent& event) {
    BOOST_LOG_TRIVIAL(debug) << "Changing current game...";
    _game = _games[event.GetId() - MENU_LowestDynamicGameID];
    try {
        _game.Init();  //In case it hasn't already been done.
        *find(_games.begin(), _games.end(), _game) = _game;  //Sync changes.
        BOOST_LOG_TRIVIAL(debug) << "New game is " << _game.Name();
    } catch (boss::error& e) {
        BOOST_LOG_TRIVIAL(error) << "Game-specific settings could not be initialised." << e.what();
        wxMessageBox(
            FromUTF8(format(loc::translate("Error: Game-specific settings could not be initialised. %1%")) % e.what()),
            translate("BOSS: Error"),
            wxOK | wxICON_ERROR,
            NULL);
    }
	SetTitle(FromUTF8("BOSS - " + _game.Name()));
}

void Launcher::OnHelp(wxCommandEvent& event) {
    //Look for file.
    BOOST_LOG_TRIVIAL(debug) << "Opening readme.";
    if (fs::exists(g_path_readme.string())) {
        wxLaunchDefaultApplication(g_path_readme.string());
    } else {  //No ReadMe exists, show a pop-up message saying so.
        BOOST_LOG_TRIVIAL(error) << "File \"" << g_path_readme.string() << "\" could not be found.";
        wxMessageBox(
            FromUTF8(format(loc::translate("Error: \"%1%\" cannot be found.")) % g_path_readme.string()),
            translate("BOSS: Error"),
            wxOK | wxICON_ERROR,
            this);
    }
}

void Launcher::OnAbout(wxCommandEvent& event) {
    BOOST_LOG_TRIVIAL(debug) << "Opening About dialog.";
    wxAboutDialogInfo aboutInfo;
    aboutInfo.SetName("BOSS");
    aboutInfo.SetVersion(IntToString(g_version_major)+"."+IntToString(g_version_minor)+"."+IntToString(g_version_patch));
    aboutInfo.SetDescription(translate(
    "A utility that optimises TES IV: Oblivion, Nehrim - At Fate's Edge,\nTES V: Skyrim, Fallout 3 and Fallout: New Vegas mod load orders."));
    aboutInfo.SetCopyright("Copyright (C) 2009-2013 BOSS Development Team.");
    aboutInfo.SetWebSite("http://code.google.com/p/better-oblivion-sorting-software/");
	aboutInfo.SetLicence("This program is free software: you can redistribute it and/or modify\n"
    "it under the terms of the GNU General Public License as published by\n"
    "the Free Software Foundation, either version 3 of the License, or\n"
    "(at your option) any later version.\n"
	"\n"
    "This program is distributed in the hope that it will be useful,\n"
    "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
    "GNU General Public License for more details.\n"
	"\n"
    "You should have received a copy of the GNU General Public License\n"
    "along with this program.  If not, see <http://www.gnu.org/licenses/>.");
	aboutInfo.SetIcon(wxIconLocation("BOSS.exe"));
    wxAboutBox(aboutInfo);
}

LoadOrderPreview::LoadOrderPreview(wxWindow *parent, const wxString title, const std::list<boss::Plugin>& plugins) : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER), _plugins(plugins) {

    //Init controls.
    _loadOrder = new wxListView(this, LIST_LoadOrder, wxDefaultPosition, wxDefaultSize, wxLC_REPORT);

    _moveUp = new wxButton(this, BUTTON_MoveUp, translate("Up"));
    _moveDown = new wxButton(this, BUTTON_MoveDown, translate("Down"));

    //Populate list.
    _loadOrder->AppendColumn(translate("Load Order"));
    size_t i=0;
    for (list<boss::Plugin>::const_iterator it=plugins.begin(), endit=plugins.end(); it != endit; ++it, ++i) {
        _loadOrder->InsertItem(i, FromUTF8(it->Name()));
    }
    _loadOrder->SetColumnWidth(0, wxLIST_AUTOSIZE);

    //Set up event handling.
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &LoadOrderPreview::OnMoveUp, this, BUTTON_MoveUp);
    Bind(wxEVT_COMMAND_BUTTON_CLICKED, &LoadOrderPreview::OnMoveDown, this, BUTTON_MoveDown);

    //Set up layout.
    wxBoxSizer * bigBox = new wxBoxSizer(wxVERTICAL);

    bigBox->Add(_loadOrder, 1, wxEXPAND|wxALL, 10);

    wxBoxSizer * hbox = new wxBoxSizer(wxHORIZONTAL);
    hbox->Add(_moveUp, 0, wxRIGHT, 5);
    hbox->Add(_moveDown, 0, wxLEFT, 5);
    bigBox->Add(hbox, 0, wxALIGN_RIGHT|wxBOTTOM|wxRIGHT, 10);

    bigBox->Add(new wxStaticText(this, wxID_ANY, translate("Please submit any alterations made for reasons other than user preference \nto the BOSS team so that they may include the changes in the masterlist.")), 0, wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM, 10);

    //Need to add 'OK' and 'Cancel' buttons.
	wxSizer * sizer = CreateSeparatedButtonSizer(wxOK|wxCANCEL);

	//Now add TabHolder and OK button to window sizer.
    if (sizer != NULL)
        bigBox->Add(sizer, 0, wxEXPAND|wxLEFT|wxBOTTOM|wxRIGHT, 15);

    //Now set the layout and sizes.
	SetBackgroundColour(wxColour(255,255,255));
    SetIcon(wxIconLocation("BOSS.exe"));
	SetSizerAndFit(bigBox);
}

void LoadOrderPreview::OnMoveUp(wxCommandEvent& event) {
    BOOST_LOG_TRIVIAL(debug) << "Moving plugin(s) up the load order.";
    long selected = _loadOrder->GetFirstSelected();

    while (selected != -1) {
        if (selected > 0) {
            wxString selectedText = _loadOrder->GetItemText(selected);
            wxString aboveText = _loadOrder->GetItemText(selected - 1);

            BOOST_LOG_TRIVIAL(trace) << "Moving plugin \"" << string(selectedText.ToUTF8());

            _loadOrder->SetItemText(selected, aboveText);
            _loadOrder->SetItemText(selected - 1, selectedText);

            _movedPlugins.insert(string(selectedText.ToUTF8()));

            _loadOrder->Select(selected, false);
            _loadOrder->Select(selected - 1, true);
        }
        selected = _loadOrder->GetNextSelected(selected);
    }
    BOOST_LOG_TRIVIAL(debug) << "Plugin(s) moved up.";
}

void LoadOrderPreview::OnMoveDown(wxCommandEvent& event) {
    BOOST_LOG_TRIVIAL(debug) << "Moving plugin(s) down the load order.";
    for (long i=_loadOrder->GetItemCount() - 1; i > -1; --i) {
        if (_loadOrder->IsSelected(i)) {
            wxString selectedText = _loadOrder->GetItemText(i);
            wxString belowText = _loadOrder->GetItemText(i + 1);

            BOOST_LOG_TRIVIAL(trace) << "Moving plugin \"" << string(selectedText.ToUTF8());

            _loadOrder->SetItemText(i, belowText);
            _loadOrder->SetItemText(i + 1, selectedText);

            _movedPlugins.insert(string(selectedText.ToUTF8()));

            _loadOrder->Select(i, false);
            _loadOrder->Select(i + 1, true);
        }
    }
    BOOST_LOG_TRIVIAL(debug) << "Plugin(s) moved down.";
}

std::list<boss::Plugin> LoadOrderPreview::GetLoadOrder() const {
    BOOST_LOG_TRIVIAL(debug) << "Getting full load order from preview window.";
    list<boss::Plugin> plugins;
    bool wasMovedUp = false;
    for (size_t i=0,max=_loadOrder->GetItemCount(); i < max; ++i) {
        string name = string(_loadOrder->GetItemText(i).ToUTF8());

        list<boss::Plugin>::const_iterator it = find(_plugins.begin(), _plugins.end(), boss::Plugin(name));

        plugins.push_back(*it);

        if (wasMovedUp) {
            BOOST_LOG_TRIVIAL(trace) << "The previous plugin was moved up in the load order, so adding it to the 'load after' set for the current plugin.";
            list<boss::Plugin>::const_iterator jt = ----plugins.end();
            set<boss::File> loadAfter = plugins.back().LoadAfter();
            loadAfter.insert(File(jt->Name()));
            plugins.back().LoadAfter(loadAfter);
            wasMovedUp = false;
        }

        if (_movedPlugins.find(name) != _movedPlugins.end()) {
            BOOST_LOG_TRIVIAL(trace) << "The current plugin was moved - checking if it was moved up or down.";
            //Check if this plugin has been moved earlier or later by comparing distances in the original list and the new one.
            size_t newDist = plugins.size() - 1;
            size_t oldDist = distance(_plugins.begin(), it);

            if (newDist > oldDist) {
                BOOST_LOG_TRIVIAL(trace) << "The current plugin was moved down, add the preceding plugin to the current plugin's 'load after' set.";
                //Record the preceding plugin in this plugin's "load after" set.
                list<boss::Plugin>::const_iterator jt = ----plugins.end();
                set<boss::File> loadAfter = plugins.back().LoadAfter();
                loadAfter.insert(File(jt->Name()));
                plugins.back().LoadAfter(loadAfter);
            } else {
                BOOST_LOG_TRIVIAL(trace) << "The current plugin was moved up.";
                //Record this plugin in the following plugin's "load after" set.
                wasMovedUp = true;
            }
        }
    }

    return plugins;
}

std::set<std::string> LoadOrderPreview::GetMovedPlugins() const {
    return _movedPlugins;
}
