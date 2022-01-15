#include "F3DLoader.h"

#include "F3DAnimationManager.h"
#include "F3DIcon.h"
#include "F3DLog.h"
#include "F3DNSDelegate.h"
#include "F3DOffscreenRender.h"
#include "F3DOptions.h"
#include "F3DReaderFactory.h"
#include "F3DReaderInstantiator.h"
#include "f3d_options.h"
#include "vtkF3DGenericImporter.h"
#include "vtkF3DInteractorEventRecorder.h"
#include "vtkF3DInteractorStyle.h"
#include "vtkF3DObjectFactory.h"
#include "vtkF3DRendererWithColoring.h"

#include <vtkCallbackCommand.h>
#include <vtkCamera.h>
#include <vtkDoubleArray.h>
#include <vtkLogger.h>
#include <vtkNew.h>
#include <vtkPNGReader.h>
#include <vtkPointGaussianMapper.h>
#include <vtkPolyDataMapper.h>
#include <vtkProgressBarRepresentation.h>
#include <vtkProgressBarWidget.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkScalarBarActor.h>
#include <vtkStringArray.h>
#include <vtkTimerLog.h>
#include <vtkVersion.h>
#include <vtksys/Directory.hxx>
#include <vtksys/SystemTools.hxx>

#include <algorithm>

namespace
{
vtkSmartPointer<vtkImporter> GetImporter(const f3d::options& options, const std::string& fileName)
{
  if (!options.get<bool>("geometry-only"))
  {
    // Try to find the first compatible reader with scene reading capabilities
    F3DReader* reader = F3DReaderFactory::GetReader(fileName);
    if (reader)
    {
      vtkSmartPointer<vtkImporter> importer = reader->CreateSceneReader(fileName);
      if (importer)
      {
        return importer;
      }
    }
  }

  // Use the generic importer and check if it can process the file
  vtkNew<vtkF3DGenericImporter> importer;
  importer->SetFileName(fileName.c_str());
  importer->SetOptions(options);
  if (!importer->CanReadFile())
  {
    return nullptr;
  }
  return importer;
}
}

typedef struct ProgressDataStruct
{
  vtkTimerLog* timer;
  vtkProgressBarWidget* widget;
} ProgressDataStruct;

class F3DLoader::F3DInternals
{
public:
  std::vector<std::string> FilesList;
  int CurrentFileIndex = 0;
  F3DOptionsParser Parser;
  F3DOptions CommandLineOptions;
  F3DOptions Options;
  f3d::options NewOptions;
  F3DAnimationManager AnimationManager;
  vtkSmartPointer<vtkF3DRenderer> Renderer;
  vtkSmartPointer<vtkImporter> Importer;
  vtkSmartPointer<vtkRenderWindow> RenWin;
  F3DReaderInstantiator ReaderInstantiator;
};

//----------------------------------------------------------------------------
F3DLoader::F3DLoader()
  : Internals(new F3DLoader::F3DInternals)
{
#if NDEBUG
  vtkObject::GlobalWarningDisplayOff();
#endif

  // Disable vtkLogger in case VTK was built with log support
  vtkLogger::SetStderrVerbosity(vtkLogger::VERBOSITY_OFF);
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 0, 20200701)
  vtkLogger::SetInternalVerbosityLevel(vtkLogger::VERBOSITY_OFF);
#endif

  // instanciate our own polydata mapper and output windows
  vtkNew<vtkF3DObjectFactory> factory;
  vtkObjectFactory::RegisterFactory(factory);
  vtkObjectFactory::SetAllEnableFlags(0, "vtkPolyDataMapper", "vtkOpenGLPolyDataMapper");

  // Make sure to initialize the output window
  // after the object factory and before the first usage.
  F3DLog::SetQuiet(false);
}

//----------------------------------------------------------------------------
F3DLoader::~F3DLoader() = default;

//----------------------------------------------------------------------------
int F3DLoader::Start(int argc, char** argv)
{
  std::vector<std::string> files;

  this->Internals->Parser.Initialize(argc, argv);
  this->Internals->CommandLineOptions = this->Internals->Parser.GetOptionsFromCommandLine(files);
  F3DLog::SetQuiet(this->Internals->CommandLineOptions.Quiet);
  this->Internals->Parser.InitializeDictionaryFromConfigFile(
    this->Internals->CommandLineOptions.UserConfigFile);

  vtkNew<vtkRenderWindowInteractor> interactor;
  if (!this->Internals->CommandLineOptions.NoRender)
  {
    this->Internals->RenWin = vtkSmartPointer<vtkRenderWindow>::New();
    this->Internals->RenWin->SetMultiSamples(0); // Disable hardware antialiasing
    this->Internals->RenWin->SetWindowName(f3d::AppTitle.c_str());

    vtkNew<vtkF3DInteractorStyle> style;
    style->SetAnimationManager(this->Internals->AnimationManager);
    // Will only be used when interacting with a animated file
    style->SetOptions(this->Internals->NewOptions);

    // Setup the observers for the interactor style events
    vtkNew<vtkCallbackCommand> newFilesCallback;
    newFilesCallback->SetClientData(this);
    newFilesCallback->SetCallback(
      [](vtkObject*, unsigned long, void* clientData, void* callData)
      {
        F3DLoader* loader = static_cast<F3DLoader*>(clientData);
        loader->Internals->CurrentFileIndex = static_cast<int>(loader->Internals->FilesList.size());
        vtkStringArray* filesArr = static_cast<vtkStringArray*>(callData);
        for (int i = 0; i < filesArr->GetNumberOfTuples(); i++)
        {
          loader->AddFile(filesArr->GetValue(i));
        }
        loader->LoadFile();
      });
    style->AddObserver(vtkF3DInteractorStyle::NewFilesEvent, newFilesCallback);

    vtkNew<vtkCallbackCommand> loadFileCallback;
    loadFileCallback->SetClientData(this);
    loadFileCallback->SetCallback(
      [](vtkObject*, unsigned long, void* clientData, void* callData)
      {
        F3DLoader* loader = static_cast<F3DLoader*>(clientData);
        int* load = static_cast<int*>(callData);
        loader->LoadFile(*load);
      });
    style->AddObserver(vtkF3DInteractorStyle::LoadFileEvent, loadFileCallback);

    vtkNew<vtkCallbackCommand> toggleAnimationCallback;
    toggleAnimationCallback->SetClientData(&this->Internals->AnimationManager);
    toggleAnimationCallback->SetCallback(
      [](vtkObject*, unsigned long, void* clientData, void*)
      {
        F3DAnimationManager* animMgr = static_cast<F3DAnimationManager*>(clientData);
        animMgr->ToggleAnimation();
      });
    style->AddObserver(vtkF3DInteractorStyle::ToggleAnimationEvent, toggleAnimationCallback);

    // Offscreen rendering must be set before initializing interactor
    if (!this->Internals->CommandLineOptions.Reference.empty() ||
      !this->Internals->CommandLineOptions.Output.empty())
    {
      this->Internals->RenWin->OffScreenRenderingOn();
    }

    interactor->SetRenderWindow(this->Internals->RenWin);
    interactor->SetInteractorStyle(style);
    interactor->Initialize();

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 0, 20200615)
    // set icon
    vtkNew<vtkPNGReader> iconReader;
    iconReader->SetMemoryBuffer(F3DIcon);
    iconReader->SetMemoryBufferLength(sizeof(F3DIcon));
    iconReader->Update();

    this->Internals->RenWin->SetIcon(iconReader->GetOutput());
#endif

#if __APPLE__
    F3DNSDelegate::InitializeDelegate(this, this->Internals->RenWin);
#endif
  }

  this->AddFiles(files);
  bool loaded = this->LoadFile();
  int retVal = EXIT_SUCCESS;

  // Actual Options been parsed in LoadFile so use them
  if (!this->Internals->NewOptions.get<bool>("no-render"))
  {
    // Manage recording options
    vtkNew<vtkF3DInteractorEventRecorder> recorder;
    std::string interactionTestRecordFile =
      this->Internals->NewOptions.get<std::string>("interaction-test-record");
    std::string interactionTestPlayFile =
      this->Internals->NewOptions.get<std::string>("interaction-test-play");
    bool record = !interactionTestRecordFile.empty();
    bool play = !interactionTestPlayFile.empty();
    if (record || play)
    {
      recorder->SetInteractor(interactor);
      if (record)
      {
        if (play)
        {
          F3DLog::Print(F3DLog::Severity::Warning,
            "Interaction test record and play files have been provided, play file ignored.");
        }
        recorder->SetFileName(interactionTestRecordFile.c_str());
        recorder->On();
        recorder->Record();
      }
      else
      {
        recorder->SetFileName(interactionTestPlayFile.c_str());
        recorder->Play();
        recorder->Off();
      }
    }

    bool noBackground = this->Internals->NewOptions.get<bool>("no-background");
    std::string reference = this->Internals->NewOptions.get<std::string>("reference");
    std::string output = this->Internals->NewOptions.get<std::string>("output");

    // Recorder can stop the interactor, make sure it is still running
    if (interactor->GetDone())
    {
      F3DLog::Print(
        F3DLog::Severity::Warning, "Interactor has been stopped, no rendering performed");
      retVal = EXIT_FAILURE;
    }
    else if (!reference.empty())
    {
      if (!loaded)
      {
        F3DLog::Print(F3DLog::Severity::Warning, "No file loaded, no rendering performed");
        retVal = EXIT_FAILURE;
      }
      else
      {
        retVal = F3DOffscreenRender::RenderTesting(this->Internals->RenWin, reference,
                   this->Internals->NewOptions.get<double>("ref-threshold"), noBackground, output)
          ? EXIT_SUCCESS
          : EXIT_FAILURE;
      }
    }
    else if (!output.empty())
    {
      if (!loaded)
      {
        F3DLog::Print(F3DLog::Severity::Warning, "No file loaded, no rendering performed");
        retVal = EXIT_FAILURE;
      }
      else
      {
        retVal = F3DOffscreenRender::RenderOffScreen(this->Internals->RenWin, output, noBackground)
          ? EXIT_SUCCESS
          : EXIT_FAILURE;
      }
    }
    else
    {
      this->Internals->RenWin->Render();
      interactor->Start();
    }

    // The axis widget should be disabled before destruction
    this->Internals->Renderer->ShowAxis(false);
    this->Internals->AnimationManager.Finalize();

    if (record)
    {
      recorder->Off();
    }
  }

  return retVal;
}

//----------------------------------------------------------------------------
void F3DLoader::AddFiles(const std::vector<std::string>& files)
{
  for (auto& file : files)
  {
    this->AddFile(file);
  }
}

//----------------------------------------------------------------------------
void F3DLoader::AddFile(const std::string& path, bool recursive)
{
  if (path.empty() || !vtksys::SystemTools::FileExists(path))
  {
    F3DLog::Print(F3DLog::Severity::Error, "File ", path, " does not exist");
    return;
  }

  std::string fullPath = vtksys::SystemTools::CollapseFullPath(path);

  if (vtksys::SystemTools::FileIsDirectory(fullPath))
  {
    if (recursive)
    {
      vtksys::Directory dir;
      dir.Load(fullPath);
      std::set<std::string> sortedFiles;

      // Sorting is necessary as KWSys can provide unsorted files
      for (unsigned long i = 0; i < dir.GetNumberOfFiles(); i++)
      {
        std::string currentFile = dir.GetFile(i);
        if (currentFile != "." && currentFile != "..")
        {
          sortedFiles.insert(currentFile);
        }
      }
      for (std::string currentFile : sortedFiles)
      {
        this->AddFile(vtksys::SystemTools::JoinPath({ "", fullPath, currentFile }), false);
      }
    }
  }
  else
  {
    auto it =
      std::find(this->Internals->FilesList.begin(), this->Internals->FilesList.end(), fullPath);

    if (it == this->Internals->FilesList.end())
    {
      this->Internals->FilesList.push_back(fullPath);
    }
  }
}

//----------------------------------------------------------------------------
// TODO this method is long and complex, this should be improved
bool F3DLoader::LoadFile(int load)
{
  // Prevent the animation manager from playing
  this->Internals->AnimationManager.Finalize();

  // Clear renderer if already present
  if (this->Internals->Renderer)
  {
    this->Internals->Renderer->ShowAxis(false);
    this->Internals->RenWin->RemoveRenderer(this->Internals->Renderer);
  }

  std::string filePath, fileInfo;
  int size = static_cast<int>(this->Internals->FilesList.size());
  if (size > 0)
  {
    // Compute the correct file index
    this->Internals->CurrentFileIndex = (this->Internals->CurrentFileIndex + load) % size;
    this->Internals->CurrentFileIndex = this->Internals->CurrentFileIndex < 0
      ? this->Internals->CurrentFileIndex + size
      : this->Internals->CurrentFileIndex;

    if (this->Internals->CurrentFileIndex >= size)
    {
      F3DLog::Print(
        F3DLog::Severity::Error, "Cannot load file index ", this->Internals->CurrentFileIndex);
      return false;
    }
    filePath = this->Internals->FilesList[this->Internals->CurrentFileIndex];
    fileInfo = "(" + std::to_string(this->Internals->CurrentFileIndex + 1) + "/" +
      std::to_string(size) + ") " + vtksys::SystemTools::GetFilenameName(filePath);
  }

  if (this->Internals->CommandLineOptions.DryRun)
  {
    this->Internals->Options = this->Internals->CommandLineOptions;
  }
  else
  {
    this->Internals->Options = this->Internals->Parser.GetOptionsFromConfigFile(filePath);
  }

  // Start working with libf3d API here
  // This should change in the future and be much higher up
  // TODO
  F3DOptionsParser::ConvertToNewAPI(this->Internals->Options, &this->Internals->NewOptions);

  F3DLog::SetQuiet(this->Internals->NewOptions.get<bool>("quiet"));

  bool verbose = this->Internals->NewOptions.get<bool>("verbose");
  bool noRender = this->Internals->NewOptions.get<bool>("no-render");
  if (verbose || noRender)
  {
    if (filePath.empty())
    {
      F3DLog::Print(F3DLog::Severity::Info, "No file to load provided\n");
    }
    else
    {
      F3DLog::Print(F3DLog::Severity::Info, "Loading: ", filePath, "\n");
    }
  }

  if (!noRender)
  {
    this->Internals->RenWin->SetSize(
      this->Internals->NewOptions.get<std::vector<int> >("resolution").data());
    this->Internals->RenWin->SetFullScreen(this->Internals->NewOptions.get<bool>("fullscreen"));
  }

  bool loaded = false;
  if (filePath.empty())
  {
    if (!noRender)
    {
      this->Internals->Renderer = vtkSmartPointer<vtkF3DRenderer>::New();
      this->Internals->RenWin->AddRenderer(this->Internals->Renderer);

      fileInfo += "No file to load provided, please drop one into this window";
      this->Internals->Renderer->Initialize(this->Internals->NewOptions, fileInfo);
      this->Internals->Renderer->ShowOptions();
    }
    return loaded;
  }

  this->Internals->Importer = ::GetImporter(this->Internals->NewOptions, filePath);
  vtkF3DGenericImporter* genericImporter =
    vtkF3DGenericImporter::SafeDownCast(this->Internals->Importer);

  vtkNew<vtkProgressBarWidget> progressWidget;
  vtkNew<vtkTimerLog> timer;
  ProgressDataStruct data;
  data.timer = timer.Get();
  data.widget = progressWidget.Get();

  if (!this->Internals->Importer)
  {
    F3DLog::Print(
      F3DLog::Severity::Warning, filePath, " is not a file of a supported file format\n");
    if (!noRender)
    {
      fileInfo += " [UNSUPPORTED]";
      this->Internals->Renderer = vtkSmartPointer<vtkF3DRenderer>::New();
      this->Internals->RenWin->AddRenderer(this->Internals->Renderer);
      this->Internals->Renderer->Initialize(this->Internals->NewOptions, fileInfo);
      this->Internals->Renderer->ShowOptions();
    }
    return loaded;
  }
  else
  {
    loaded = true;
  }

  if (!noRender)
  {
    // Create and initialize renderer
    if (genericImporter)
    {
      this->Internals->Renderer = vtkSmartPointer<vtkF3DRendererWithColoring>::New();
    }
    else
    {
      this->Internals->Renderer = vtkSmartPointer<vtkF3DRenderer>::New();
    }
    this->Internals->RenWin->AddRenderer(this->Internals->Renderer);
    this->Internals->Renderer->Initialize(this->Internals->NewOptions, fileInfo);

    this->Internals->Importer->SetRenderWindow(this->Internals->Renderer->GetRenderWindow());

#if VTK_VERSION_NUMBER > VTK_VERSION_CHECK(9, 0, 20210228)
    this->Internals->Importer->SetCamera(this->Internals->NewOptions.get<int>("camera-index"));
#endif

    if (this->Internals->NewOptions.get<bool>("progress"))
    {
      vtkNew<vtkCallbackCommand> progressCallback;
      progressCallback->SetClientData(&data);
      progressCallback->SetCallback(
        [](vtkObject*, unsigned long, void* clientData, void* callData)
        {
          auto progressData = static_cast<ProgressDataStruct*>(clientData);
          progressData->timer->StopTimer();
          vtkProgressBarWidget* widget = progressData->widget;
          // Only show and render the progress bar if loading takes more than 0.15 seconds
          if (progressData->timer->GetElapsedTime() > 0.15)
          {
            widget->On();
            vtkProgressBarRepresentation* rep =
              vtkProgressBarRepresentation::SafeDownCast(widget->GetRepresentation());
            rep->SetProgressRate(*static_cast<double*>(callData));
            widget->GetInteractor()->GetRenderWindow()->Render();
          }
        });
      this->Internals->Importer->AddObserver(vtkCommand::ProgressEvent, progressCallback);

      progressWidget->SetInteractor(this->Internals->Renderer->GetRenderWindow()->GetInteractor());

      vtkProgressBarRepresentation* progressRep =
        vtkProgressBarRepresentation::SafeDownCast(progressWidget->GetRepresentation());
      progressRep->SetProgressRate(0.0);
      progressRep->ProportionalResizeOff();
      progressRep->SetPosition(0.0, 0.0);
      progressRep->SetPosition2(1.0, 0.0);
      progressRep->SetMinimumSize(0, 5);
      progressRep->SetProgressBarColor(1, 1, 1);
      progressRep->DrawBackgroundOff();
      progressRep->DragableOff();
      progressRep->SetShowBorderToOff();

#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 0, 20201027)
      progressRep->DrawFrameOff();
      progressRep->SetPadding(0.0, 0.0);
#endif
    }
  }

  timer->StartTimer();
  this->Internals->Importer->Update();

  // we need to remove progress observer in order to hide the progress bar during animation
  this->Internals->Importer->RemoveObservers(vtkCommand::ProgressEvent);

  if (!noRender)
  {
    this->Internals->AnimationManager.Initialize(this->Internals->NewOptions,
      this->Internals->Importer, this->Internals->RenWin, this->Internals->Renderer);
  }

  // Display description
  if (verbose || noRender)
  {
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 0, 20210228)
    vtkIdType availCameras = this->Internals->Importer->GetNumberOfCameras();
    if (availCameras <= 0)
    {
      F3DLog::Print(F3DLog::Severity::Info, "No camera available in this file");
    }
    else
    {
      F3DLog::Print(F3DLog::Severity::Info, "Camera(s) available in this file are:");
    }
    for (int i = 0; i < availCameras; i++)
    {
      F3DLog::Print(F3DLog::Severity::Info, i, ": ", this->Internals->Importer->GetCameraName(i));
    }
    F3DLog::Print(F3DLog::Severity::Info, "\n");
#endif
    F3DLog::Print(F3DLog::Severity::Info, this->Internals->Importer->GetOutputsDescription());
  }

  if (!noRender)
  {
    progressWidget->Off();

    // Recover generic importer specific actors and mappers to set on the renderer with coloring
    if (genericImporter)
    {
      // no sanity test needed
      vtkF3DRendererWithColoring* renWithColor =
        vtkF3DRendererWithColoring::SafeDownCast(this->Internals->Renderer);

      renWithColor->SetScalarBarActor(genericImporter->GetScalarBarActor());
      renWithColor->SetGeometryActor(genericImporter->GetGeometryActor());
      renWithColor->SetPointSpritesActor(genericImporter->GetPointSpritesActor());
      renWithColor->SetVolumeProp(genericImporter->GetVolumeProp());
      renWithColor->SetPolyDataMapper(genericImporter->GetPolyDataMapper());
      renWithColor->SetPointGaussianMapper(genericImporter->GetPointGaussianMapper());
      renWithColor->SetVolumeMapper(genericImporter->GetVolumeMapper());
      renWithColor->SetColoring(genericImporter->GetPointDataForColoring(),
        genericImporter->GetCellDataForColoring(), this->Internals->NewOptions.get<bool>("cells"),
        genericImporter->GetArrayIndexForColoring(),
        this->Internals->NewOptions.get<int>("component"));
    }

    // Actors are loaded, use the bounds to reset camera and set-up SSAO
    this->Internals->Renderer->SetupRenderPasses();
    this->Internals->Renderer->UpdateInternalActors();
    this->Internals->Renderer->ShowOptions();

    // Set the initial camera once all options
    // have been shown as they may have an effect on it
    if (this->Internals->NewOptions.get<int>("camera-index") < 0)
    {
      // set a default camera from bounds using VTK method
      this->Internals->Renderer->vtkRenderer::ResetCamera();

      // use options to overwrite camera parameters
      vtkCamera* cam = this->Internals->Renderer->GetActiveCamera();

      std::vector<double> cameraPosition =
        this->Internals->NewOptions.get<std::vector<double> >("camera-position");
      if (cameraPosition.size() == 3)
      {
        cam->SetPosition(cameraPosition.data());
      }

      std::vector<double> cameraFocalPoint =
        this->Internals->NewOptions.get<std::vector<double> >("camera-focal-point");
      if (cameraFocalPoint.size() == 3)
      {
        cam->SetFocalPoint(cameraFocalPoint.data());
      }

      std::vector<double> cameraViewUp =
        this->Internals->NewOptions.get<std::vector<double> >("camera-view-up");
      if (cameraViewUp.size() == 3)
      {
        cam->SetViewUp(cameraViewUp.data());
      }

      double cameraViewAngle = this->Internals->NewOptions.get<double>("camera-view-angle");
      if (cameraViewAngle != 0)
      {
        cam->SetViewAngle(cameraViewAngle);
      }

      cam->Azimuth(this->Internals->NewOptions.get<double>("camera-azimuth-angle"));
      cam->Elevation(this->Internals->NewOptions.get<double>("camera-elevation-angle"));
      cam->OrthogonalizeViewUp();

      if (verbose)
      {
        double* position = cam->GetPosition();
        F3DLog::Print(F3DLog::Severity::Info, "Camera position is: ", position[0], ", ",
          position[1], ", ", position[2], ".");
        double* focalPoint = cam->GetFocalPoint();
        F3DLog::Print(F3DLog::Severity::Info, "Camera focal point is: ", focalPoint[0], ", ",
          focalPoint[1], ", ", focalPoint[2], ".");
        double* viewUp = cam->GetViewUp();
        F3DLog::Print(F3DLog::Severity::Info, "Camera view up is: ", viewUp[0], ", ", viewUp[1],
          ", ", viewUp[2], ".");
        F3DLog::Print(F3DLog::Severity::Info, "Camera view angle is: ", cam->GetViewAngle(), ".\n");
      }
    }

    this->Internals->Renderer->InitializeCamera();
  }
  return loaded;
}