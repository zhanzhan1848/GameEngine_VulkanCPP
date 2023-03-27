using PrimalEditor.GameProject;
using PrimalEditor.Ultilities;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
//using Microsoft.VisualStudio;
//using Microsoft.VisualStudio.OLE.Interop;
using System.Text;
using System.Threading.Tasks;

namespace PrimalEditor.GameDev
{
    static class VisualStudio
    {
        public static bool BuildSucceeded { get; private set; } = true;

        private static EnvDTE80.DTE2 _vsInstance = null;
        private static readonly string _progID = "VisualStudio.DTE.17.0";

        [DllImport("ole32.dll")]
        private static extern int CreateBindCtx(uint reserverd, out IBindCtx ppbc);

        [DllImport("ole32.dll")]
        private static extern int GetRunningObjectTable(uint reserverd, out IRunningObjectTable pprot);
        public static void OpenVisualStudio(string solutionPath)
        {
            IRunningObjectTable rot = null;
            IEnumMoniker monikerTable = null;
            IBindCtx bindCtx = null;
            try
            {
                if (_vsInstance == null)
                {
                    // Finde and open visual
                    var hResult = GetRunningObjectTable(0, out rot);
                    if (hResult < 0 || rot == null) throw new COMException($"GetRunningObjectTable() returned HRESULT: {hResult:X8}");

                    rot.EnumRunning(out monikerTable);
                    monikerTable.Reset();

                    hResult = CreateBindCtx(0, out bindCtx);
                    if (hResult < 0 || bindCtx == null) throw new COMException($"CreateBindCtx() returned HRESULY: {hResult:X8}");

                    IMoniker[] currentMoniker = new IMoniker[1];
                    while(monikerTable.Next(1, currentMoniker, IntPtr.Zero) == 0)
                    {
                        string name = string.Empty;
                        currentMoniker[0]?.GetDisplayName(bindCtx, null, out name);
                        if(name.Contains(_progID))
                        {
                            hResult = rot.GetObject(currentMoniker[0], out object obj);
                            if (hResult < 0 || obj == null) throw new COMException($"Running object table's GetObject() returned HRESULT:{hResult:X8}");

                            EnvDTE80.DTE2 dte = obj as EnvDTE80.DTE2;
                            var solutionName = dte.Solution.FullName;
                            if(solutionName == solutionPath)
                            {
                                _vsInstance = dte;
                                Debug.WriteLine(_vsInstance.ToString());
                                break;
                            }
                        }
                    }

                    if (_vsInstance == null)
                    {
                        Type visualStudioType = Type.GetTypeFromProgID(_progID, true);
                        _vsInstance = Activator.CreateInstance(visualStudioType) as EnvDTE80.DTE2;
                    }
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine(ex.Message);
                Logger.Log(MessageType.Error, "Failed to open visual studio");
            }
            finally
            {
                if(monikerTable != null) Marshal.ReleaseComObject(monikerTable);
                if(rot != null) Marshal.ReleaseComObject(rot);
                if(bindCtx != null) Marshal.ReleaseComObject(bindCtx);
            }
        }

        public static void CloseVisualStudio()
        {
            if(_vsInstance?.Solution.IsOpen == true)
            {
                _vsInstance.ExecuteCommand("File.SaveAll");
                _vsInstance.Solution.Close(true);
            }
            _vsInstance?.Quit();
            _vsInstance = null;
        }

        public static bool AddFilesToSolution(string solution, string projectName, string[] files)
        {
            System.Diagnostics.Debug.Assert(files?.Length > 0);
            OpenVisualStudio(solution);
            try
            {
                if (_vsInstance != null)
                {
                    if (!_vsInstance.Solution.IsOpen) _vsInstance.Solution.Open(solution);
                    else _vsInstance.ExecuteCommand("File.SaveAll");

                    foreach (EnvDTE.Project project in _vsInstance.Solution.Projects)
                    {
                        if (project.UniqueName.Contains(projectName))
                        {
                            foreach (var file in files)
                            {
                                project.ProjectItems.AddFromFile(file);
                            }
                        }
                    }

                    var cpp = files.FirstOrDefault(x => Path.GetExtension(x) == ".cpp");
                    if (!string.IsNullOrEmpty(cpp))
                    {
                        _vsInstance.ItemOperations.OpenFile(cpp, "{7651A703-06E5-11D1-8EBD-00A0C90F26EA}").Visible = true; //"{7651A703-06E5-11D1-8EBD-00A0C90F26EA}"
                        //EnvDTE.Constants.vsViewKindTextView
                    }
                    _vsInstance.MainWindow.Activate();
                    _vsInstance.MainWindow.Visible = true;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine(ex.Message);
                System.Diagnostics.Debug.WriteLine("Failed to add files to Visual Studio project.");
                return false;
            }
            return true;
        }

        public static bool IsDebugging()
        {
            bool result = false;

            try
            {
                result = _vsInstance != null &&
                    (_vsInstance.Debugger.CurrentProgram != null || _vsInstance.Debugger.CurrentMode == EnvDTE.dbgDebugMode.dbgRunMode);
            }
            catch(Exception ex)
            {
                System.Diagnostics.Debug.Write(ex.Message);
                if (!result) System.Threading.Thread.Sleep(1000);
            }
            return result;
        }

        public static void BuildSolution(Project project, string buildConfig)
        {
            if(IsDebugging())
            {
                Logger.Log(MessageType.Error, "Visual Studio is currenty running a process.");
                return;
            }
        }
    }
}
