/*
    This source file is part of Rigs of Rods
    Copyright 2005-2012 Pierre-Michel Ricordel
    Copyright 2007-2012 Thomas Fischer
    Copyright 2013-2017 Petr Ohlidal & contributors

    For more information, see http://www.rigsofrods.org/

    Rigs of Rods is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 3, as
    published by the Free Software Foundation.

    Rigs of Rods is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Rigs of Rods. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Application.h"
#include "RoRPrerequisites.h"
#include "MainMenu.h"
#include "Language.h"
#include "ErrorUtils.h"
#include "Utils.h"
#include "PlatformUtils.h"
#include "Settings.h"

#include "Application.h"
#include "Beam.h"
#include "BeamEngine.h"
#include "BeamFactory.h"
#include "CacheSystem.h"
#include "CameraManager.h"
#include "Character.h"
#include "CharacterFactory.h"
#include "ChatSystem.h"
#include "ContentManager.h"
#include "DashBoardManager.h"
#include "DustManager.h"
#include "ErrorUtils.h"
#include "ForceFeedback.h"
#include "GlobalEnvironment.h"
#include "GUIManager.h"
#include "GUI_LoadingWindow.h"
#include "GUI_MainSelector.h"
#include "GUI_MultiplayerClientList.h"
#include "GUI_MultiplayerSelector.h"
#include "Heathaze.h"
#include "InputEngine.h"
#include "Language.h"
#include "MumbleIntegration.h"

#include "Network.h"
#include "OgreSubsystem.h"
#include "OverlayWrapper.h"
#include "OutProtocol.h"

#include "RoRFrameListener.h"
#include "Scripting.h"
#include "Settings.h"
#include "Skidmark.h"
#include "SoundScriptManager.h"
#include "SurveyMapManager.h"
#include "TerrainManager.h"
#include "Utils.h"
#include "SkyManager.h"

#include <OgreException.h>
#include <OgreRoot.h>

#ifdef USE_CURL
#   include <curl/curl.h>
#endif //USE_CURL

GlobalEnvironment* gEnv;         // Global pointer used throughout the game. Declared in "RoRPrerequisites.h". TODO: Eliminate
GlobalEnvironment  gEnvInstance; // The actual instance

#ifdef __cplusplus
extern "C" {
#endif

int main(int argc, char *argv[])
{
    using namespace RoR;

#ifdef USE_CURL
    curl_global_init(CURL_GLOBAL_ALL); // MUST init before any threads are started
#endif

    try
    {
        gEnv = &gEnvInstance;

        // ### Detect system paths ###

        int res = RoR::System::DetectBasePaths(); // Updates globals
        if (res == -1)
        {
            ErrorUtils::ShowError(_L("Startup error"), _L("Error while retrieving program directory path"));
            return -1;
        }
        else if (res == -2)
        {
            ErrorUtils::ShowError(_L("Startup error"), _L("Error while retrieving user directory path"));
            return -1;
        }

        // ### Create OGRE default logger early. ###

        Str<300> logs_dir;
        logs_dir << App::sys_user_dir.GetActive() << PATH_SLASH << "logs";
        if (!PlatformUtils::FolderExists(App::sys_logs_dir.GetActive()))
            PlatformUtils::CreateFolder(App::sys_logs_dir.GetActive());
        App::sys_logs_dir.SetActive(logs_dir);

        auto ogre_log_manager = OGRE_NEW Ogre::LogManager();
        Str<300> rorlog_path;
        rorlog_path << logs_dir << PATH_SLASH << "RoR.log";
        ogre_log_manager->createLog(Ogre::String(rorlog_path), true, true);
        App::diag_trace_globals.SetActive(true); // We have logger -> we can trace.

        // ### Setup program paths ###

        if (! Settings::SetupAllPaths()) // Updates globals
        {
            ErrorUtils::ShowError(_L("Startup error"), _L("Resources folder not found. Check if correctly installed."));
            return -1;
        }

        Settings::getSingleton().LoadRoRCfg(); // Main config file - path obtained from GVars

        if (!PlatformUtils::FolderExists(App::sys_cache_dir.GetActive()))
            PlatformUtils::CreateFolder(App::sys_cache_dir.GetActive());

        // ### Process command-line arguments ###

#if OGRE_PLATFORM != OGRE_PLATFORM_APPLE //MacOSX adds an extra argument in the form of -psn_0_XXXXXX when the app is double clicked
        Settings::getSingleton().ProcessCommandLine(argc, argv);
#endif

        if (App::app_state.GetPending() == AppState::PRINT_HELP_EXIT)
        {
            ShowCommandLineUsage();
            return 0;
        }
        if (App::app_state.GetPending() == AppState::PRINT_VERSION_EXIT)
        {
            ShowVersion();
            return 0;
        }

#ifdef USE_CRASHRPT
        InstallCrashRpt();
#endif //USE_CRASHRPT

        App::StartOgreSubsystem();
    #ifdef ROR_USE_OGRE_1_9
        Ogre::OverlaySystem* overlay_system = new Ogre::OverlaySystem(); //Overlay init
    #endif

        App::CreateContentManager();

        LanguageEngine::getSingleton().setup();

        // Add startup resources
        App::GetContentManager()->AddResourcePack(ContentManager::ResourcePack::OGRE_CORE);
        App::GetContentManager()->AddResourcePack(ContentManager::ResourcePack::WALLPAPERS);

        // Setup rendering (menu + simulation)
        Ogre::SceneManager* scene_manager = App::GetOgreSubsystem()->GetOgreRoot()->createSceneManager(Ogre::ST_EXTERIOR_CLOSE, "main_scene_manager");
        gEnv->sceneManager = scene_manager;
    #ifdef ROR_USE_OGRE_1_9
        if (overlay_system)
        {
            scene_manager->addRenderQueueListener(overlay_system);
        }
    #endif

        Ogre::Camera* camera = scene_manager->createCamera("PlayerCam");
        camera->setPosition(Ogre::Vector3(128, 25, 128)); // Position it at 500 in Z direction
        camera->lookAt(Ogre::Vector3(0, 0, -300)); // Look back along -Z
        camera->setNearClipDistance(0.5);
        camera->setFarClipDistance(1000.0 * 1.733);
        camera->setFOVy(Ogre::Degree(60));
        camera->setAutoAspectRatio(true);
        App::GetOgreSubsystem()->GetViewport()->setCamera(camera);
        gEnv->mainCamera = camera;

        Ogre::String menu_wallpaper_texture_name = GUIManager::getRandomWallpaperImage();

        App::CreateCacheSystem(); // Reads GVars

        App::GetContentManager()->OnApplicationStartup();

        App::CreateGuiManagerIfNotExists();

        // Load and show menu wallpaper
        MyGUI::VectorWidgetPtr v = MyGUI::LayoutManager::getInstance().loadLayout("wallpaper.layout");
        MyGUI::Widget* menu_wallpaper_widget = nullptr;
        if (!v.empty())
        {
            MyGUI::Widget* mainw = v.at(0);
            if (mainw)
            {
                MyGUI::ImageBox* img = (MyGUI::ImageBox *)(mainw->getChildAt(0));
                if (img)
                    img->setImageTexture(menu_wallpaper_texture_name);
                menu_wallpaper_widget = mainw;
            }
        }

#ifdef USE_ANGELSCRIPT
        new ScriptEngine(); // Init singleton. TODO: Move under Application
#endif

        App::CreateInputEngine();
        App::GetInputEngine()->setupDefault(App::GetOgreSubsystem()->GetMainHWND());

        // Initialize "managed materials"
        // These are base materials referenced by user content
        // They must be initialized before any content is loaded, including mod-cache update.
        // Otherwise material links are unresolved and loading ends with an exception
        // TODO: Study Ogre::ResourceLoadingListener and implement smarter solution (not parsing materials on cache refresh!)
        App::GetContentManager()->InitManagedMaterials();

        if (BSETTING("regen-cache-only", false)) //Can be usefull so we will leave it here -max98
        {
            App::GetContentManager()->RegenCache();
            App::app_state.SetPending(AppState::SHUTDOWN);
        }

        App::GetCacheSystem()->Startup();

        RoR::ForceFeedback force_feedback;
#ifdef _WIN32
        if (App::io_ffb_enabled.GetActive()) // Force feedback
        {
            if (App::GetInputEngine()->getForceFeedbackDevice())
            {
                force_feedback.Setup();
            }
            else
            {
                LOG("No force feedback device detected, disabling force feedback");
                App::io_ffb_enabled.SetActive(false);
            }
        }
#endif // _WIN32

#ifdef USE_MPLATFORM
	    m_frame_listener->m_platform = new MPlatform_FD();
	    if (m_frame_listener->m_platform)
	    {
		    m_platform->connect();
	    }
#endif

        RoR::App::GetInputEngine()->windowResized(App::GetOgreSubsystem()->GetRenderWindow());

        MainMenu main_obj;
        SkidmarkConfig skidmark_conf; // Loads 'skidmark.cfg' in constructor

        // ### Main loop (switches application states) ###

        AppState prev_app_state = App::app_state.GetActive();
        App::app_state.SetPending(AppState::MAIN_MENU);

        if (! App::diag_preset_terrain.IsActiveEmpty())
        {
            App::app_state.SetPending(AppState::SIMULATION);
        }

        if (App::mp_join_on_startup.GetActive() == true)
        {
            App::mp_state.SetPending(RoR::MpState::CONNECTED);
        }

        while (App::app_state.GetPending() != AppState::SHUTDOWN)
        {
            if (App::app_state.GetPending() == AppState::MAIN_MENU)
            {
                App::app_state.ApplyPending();

                if (prev_app_state == AppState::SIMULATION)
                {
#ifdef USE_SOCKETW
                    if (App::mp_state.GetActive() == MpState::CONNECTED)
                    {
                        RoR::Networking::Disconnect();
                        App::GetGuiManager()->SetVisible_MpClientList(false);
                    }
#endif
                    gEnv->cameraManager->OnReturnToMainMenu();
                    /* Hide top menu */
                    App::GetGuiManager()->SetVisible_TopMenubar(false);
                    /* Restore wallpaper */
                    menu_wallpaper_widget->setVisible(true);

#ifdef USE_MUMBLE
                    if (App::GetMumble() != nullptr)
                        App::GetMumble()->SetNonPositionalAudio();
#endif // USE_MUMBLE
                }

#ifdef USE_OPENAL
                if (App::audio_menu_music.GetActive())
                {
                    SoundScriptManager::getSingleton().createInstance("tracks/main_menu_tune", -1, nullptr);
                    SOUND_START(-1, SS_TRIG_MAIN_MENU);
                }
#endif // USE_OPENAL

                App::GetGuiManager()->ReflectGameState();
                if (App::mp_state.GetPending() == MpState::CONNECTED || BSETTING("SkipMainMenu", false))
                {
                    // Multiplayer started from configurator / MainMenu disabled -> go directly to map selector (traditional behavior)
                    if (App::diag_preset_terrain.IsActiveEmpty())
                    {
                        App::GetGuiManager()->SetVisible_GameMainMenu(false);
                        App::GetGuiManager()->GetMainSelector()->Show(LT_Terrain);
                    }
                }

                main_obj.EnterMainMenuLoop();
            }
            else if (App::app_state.GetPending() == AppState::SIMULATION)
            {
                {
                    RoRFrameListener sim_controller(&force_feedback, &skidmark_conf);
                    if (sim_controller.SetupGameplayLoop())
                    {
                        App::app_state.ApplyPending();
                        App::GetGuiManager()->ReflectGameState();
                        App::SetSimController(&sim_controller);
                        sim_controller.EnterGameplayLoop();
                        App::SetSimController(nullptr);
                    }
                    else
                    {
                        App::app_state.SetPending(AppState::MAIN_MENU);
                    }
                }
                gEnv->sceneManager->clearScene(); // Wipe the scene after RoRFrameListener was destroyed (->cleanups invoked)
            }
            else if (App::app_state.GetPending() == AppState::CHANGE_MAP)
            {
                //Sim -> change map -> sim
                //                  -> back to menu

                App::app_state.ApplyPending();
                menu_wallpaper_widget->setVisible(true);

                if (App::diag_preset_terrain.IsActiveEmpty())
                {
                    App::GetGuiManager()->GetMainSelector()->Show(LT_Terrain);
                }
                else
                {
                    App::GetGuiManager()->SetVisible_GameMainMenu(true);
                }
                //It's the same thing so..
                main_obj.EnterMainMenuLoop();
            }
            prev_app_state = App::app_state.GetActive();
        } // End of app state loop

        // ========================================================================
        // Cleanup
        // ========================================================================

        Settings::getSingleton().SaveSettings(); // Save RoR.cfg

        App::GetGuiManager()->GetMainSelector()->~MainSelector();

#ifdef USE_SOCKETW
        if (App::mp_state.GetActive() == MpState::CONNECTED)
        {
            RoR::Networking::Disconnect();
        }
#endif //SOCKETW

        //TODO: we should destroy OIS here
        //TODO: we could also try to destroy SoundScriptManager, but we don't care!

#ifdef USE_MPLATFORM
	    if (frame_listener->mplatform != nullptr)
	    {
		    if (frame_listener->mplatform->disconnect())
		    {
			    delete(frame_listener->mplatform);
			    frame_listener->mplatform = nullptr;
		    }
	    }
#endif

        scene_manager->destroyCamera(camera);
        App::GetOgreSubsystem()->GetOgreRoot()->destroySceneManager(scene_manager);

        App::DestroyOverlayWrapper();

        App::DestroyContentManager();

        delete gEnv->cameraManager;
        gEnv->cameraManager = nullptr;
    }
    catch (Ogre::Exception& e)
    {
        ErrorUtils::ShowError(_L("An exception has occured!"), e.getFullDescription());
    }
    catch (std::runtime_error& e)
    {
        ErrorUtils::ShowError(_L("An exception (std::runtime_error) has occured!"), e.what());
    }

#ifdef USE_CRASHRPT
    UninstallCrashRpt();
#endif //USE_CRASHRPT

    return 0;
}

#if OGRE_PLATFORM == OGRE_PLATFORM_WIN32
INT WINAPI WinMain( HINSTANCE hInst, HINSTANCE, LPSTR strCmdLine, INT )
{
    return main(__argc, __argv);
}
#endif

#ifdef __cplusplus
}
#endif
