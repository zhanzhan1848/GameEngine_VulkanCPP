using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Shapes;
using Microsoft.VisualStudio.OLE.Interop;
using OpenTK;
using OpenTK.Graphics.OpenGL4;
using OpenTK.Mathematics;
using OpenTK.Wpf;

namespace PrimalEditor.Graphics
{
    /// <summary>
    /// OpenTK_View.xaml 的交互逻辑
    /// </summary>
    public partial class OpenTK_View : UserControl
    {
        private static string shaderFilePath = $"C:/Users/zy/Desktop/PrimalMerge/PrimalEngine/PrimalEditor/Graphics/Shader/GLSL/";
        private readonly float[] _vertives =
        {
            0.5f, 0.5f, 0.0f,
            0.5f, -0.5f, 0.0f,
            -0.5f, -0.5f, 0.0f,
            -0.5f, 0.5f, 0.0f
        };

        private readonly uint[] _indices =
        {
            0, 1, 3,
            1, 2, 3
        };
        private int         _vertexBufferObject;
        private int         _vertexArrayObject;
        private Shader      _shader;
        private int         _elementBufferObject;
        private Size?       _previousSize;

        public OpenTK_View()
        {
            InitializeComponent();
            this.SizeChanged += OpenTkView_SizeChanged;
            var settings = new GLWpfControlSettings
            {
                MajorVersion = 4, MinorVersion = 3,
            };

            OpenTkView.Start(settings);
        }

        private void OpenTkView_Loaded(object sender, RoutedEventArgs e)
        {
            GL.ClearColor(Color4.Black);
            _vertexBufferObject = GL.GenBuffer();
            GL.BindBuffer(BufferTarget.ArrayBuffer, _vertexBufferObject);
            GL.BufferData(BufferTarget.ArrayBuffer, _vertives.Length * sizeof(float), _vertives, BufferUsageHint.StaticDraw);

            _vertexArrayObject = GL.GenVertexArray();
            GL.BindVertexArray(_vertexArrayObject);
            GL.VertexAttribPointer(0, 3, VertexAttribPointerType.Float, false, 3 * sizeof(float), 0);
            GL.EnableVertexAttribArray(0);

            _elementBufferObject = GL.GenBuffer();
            GL.BindBuffer(BufferTarget.ElementArrayBuffer, _elementBufferObject);
            GL.BufferData(BufferTarget.ElementArrayBuffer, _indices.Length * sizeof(uint), _indices, BufferUsageHint.StaticDraw);

            _shader = new Shader($"{shaderFilePath}/shader.vert", $"{shaderFilePath}/shader.frag");
            _shader.Use();
        }

        private void OpenTkView_OnRender(TimeSpan delta)
        {
            GL.Clear(ClearBufferMask.ColorBufferBit | ClearBufferMask.DepthBufferBit);
            // GL.Viewport(0, 0, Convert.ToInt32(this.Width), Convert.ToInt32(this.Height));

            GL.BindVertexArray(_vertexArrayObject);
            GL.DrawElements(PrimitiveType.Triangles, _indices.Length, DrawElementsType.UnsignedInt, 0);
            
            
        }

        private void OpenTkView_SizeChanged(object sender, SizeChangedEventArgs e)
        {
            if(_previousSize == null || _previousSize != e.NewSize)
            {
                GL.Viewport(0, 0, (int)e.NewSize.Width, (int)e.NewSize.Width);
                _previousSize = e.NewSize;
            }
        }

        public class Shader
        {
            int Handle;

            private bool disposedValue = false;

            public Shader(string vertexPath, string fragPath)
            {
                string VertexShaderSource = File.ReadAllText(vertexPath);
                string FragmentShaderSource = File.ReadAllText(fragPath);
                int VertexShader = GL.CreateShader(ShaderType.VertexShader);
                GL.ShaderSource(VertexShader, VertexShaderSource);
                int FragmentShader = GL.CreateShader(ShaderType.FragmentShader);
                GL.ShaderSource(FragmentShader, FragmentShaderSource);
                int vertexResult = result(VertexShader);
                if(vertexResult == 0)
                {
                    string infoLog = GL.GetShaderInfoLog(VertexShader);
                    Console.WriteLine(infoLog);
                }

                int fragResult = result(FragmentShader);
                if (fragResult == 0)
                {
                    string infoLog = GL.GetShaderInfoLog(FragmentShader);
                    Console.WriteLine(infoLog);
                }

                this.Handle = GL.CreateProgram();
                GL.AttachShader(this.Handle, VertexShader);
                GL.AttachShader(this.Handle, FragmentShader);
                GL.LinkProgram(this.Handle);

                GL.GetProgram(this.Handle, GetProgramParameterName.LinkStatus, out int success);
                if (success == 0)
                {
                    string infoLog = GL.GetProgramInfoLog(this.Handle);
                    Console.WriteLine(infoLog);
                }

                GL.DetachShader(this.Handle, VertexShader);
                GL.DetachShader(this.Handle, FragmentShader);
                GL.DeleteShader(FragmentShader);
                GL.DeleteShader(VertexShader);
            }

            public void Use()
            {
                GL.UseProgram(this.Handle);
            }

            private int result(int shader)
            {
                GL.CompileShader(shader);
                GL.GetShader(shader, ShaderParameter.CompileStatus, out int success);
                return success;
            }
            protected virtual void Dispose(bool disposing)
            {
                if (!disposedValue)
                {
                    GL.DeleteProgram(Handle);

                    disposedValue = true;
                }
            }

            ~Shader()
            {
                if (disposedValue == false)
                {
                    Console.WriteLine("GPU Resource leak! Did you forget to call Dispose()?");
                }
            }

            public void Dispose()
            {
                Dispose(true);
                GC.SuppressFinalize(this);
            }
        }
    }
}
