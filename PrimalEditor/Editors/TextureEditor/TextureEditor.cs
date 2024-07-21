﻿using PrimalEditor.Content;
using PrimalEditor.DllWrappers;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Media.Imaging;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace PrimalEditor.Editors
{
    class TextureEditor : ViewModelBase, IAssetEditor
    {
        private readonly List<List<List<BitmapSource>>> _sliceBitmaps = new();
        private List<List<List<Slice>>> _slices;

        public ICommand SetAllChannelsCommand { get; init; }
        public ICommand SetChannelCommand { get; init; }
        public ICommand RegenerateBitmapsCommand { get; init; }
        public ICommand ReimportCommand { get; init; }
        public ICommand SaveCommand { get; init; }

        private AssetEditorState _state;
        public AssetEditorState State
        {
            get => _state;
            private set
            {
                if (_state != value)
                {
                    _state = value;
                    OnPropertyChanged(nameof(State));
                }
            }
        }

        public Guid AssetGuid { get; private set; }

        private bool _canSaveChanges;
        public bool CanSaveChanges
        {
            get => _canSaveChanges;
            set
            {
                if(_canSaveChanges != value)
                {
                    _canSaveChanges = value;
                    OnPropertyChanged(nameof(CanSaveChanges));
                }
            }
        }

        private bool _isRedChannelSelected = true;
        public bool IsRedChannelSelected
        {
            get => _isRedChannelSelected;
            set
            {
                if(value != _isRedChannelSelected)
                {
                    _isRedChannelSelected = value;
                    OnPropertyChanged(nameof(IsRedChannelSelected));
                    SetImageChannels();
                }
            }
        }

        private bool _isGreenChannelSelected = true;
        public bool IsGreenChannelSelected
        {
            get => _isGreenChannelSelected;
            set
            {
                if (value != _isGreenChannelSelected)
                {
                    _isGreenChannelSelected = value;
                    OnPropertyChanged(nameof(IsGreenChannelSelected));
                    SetImageChannels();
                }
            }
        }

        private bool _isBlueChannelSelected = true;
        public bool IsBlueChannelSelected
        {
            get => _isBlueChannelSelected;
            set
            {
                if (value != _isBlueChannelSelected)
                {
                    _isBlueChannelSelected = value;
                    OnPropertyChanged(nameof(IsBlueChannelSelected));
                    SetImageChannels();
                }
            }
        }

        private bool _isAlphaChannelSelected = true;
        public bool IsAlphaChannelSelected
        {
            get => _isAlphaChannelSelected;
            set
            {
                if (value != _isAlphaChannelSelected)
                {
                    _isAlphaChannelSelected = value;
                    OnPropertyChanged(nameof(IsAlphaChannelSelected));
                    SetImageChannels();
                }
            }
        }

        public Color Channels => new()
        {
            ScR = IsRedChannelSelected ? 1.0f : 0.0f,
            ScG = IsGreenChannelSelected ? 1.0f : 0.0f,
            ScB = IsBlueChannelSelected ? 1.0f : 0.0f,
            ScA = IsAlphaChannelSelected ? 1.0f : 0.0f,
        };

        public float Stride => (float?)SelectedSliceBitmap?.Format.BitsPerPixel / 8 ?? 1.0f;

        Asset IAssetEditor.Asset => Texture;

        private Texture _texture = new();
        public Texture Texture
        {
            get => _texture;
            private set
            {
                if (_texture != value)
                {
                    _texture = value;
                    if (_texture != null)
                    {
                        IAssetImportSettings.CopyImportSettings(_texture.ImportSettings, ImportSettings);
                    }
                    OnPropertyChanged(nameof(Texture));
                    SetSelectedBitmap();
                    SetImageChannels();
                }
            }
        }

        public TextureImportSettings ImportSettings { get; } = new();

        public int MaxMipIndex => _sliceBitmaps.Any() && _sliceBitmaps.First().Any() ? _sliceBitmaps.First().Count - 1 : 0;
        public int MaxArrayIndex => _sliceBitmaps.Any() ? _sliceBitmaps.Count - 1 : 0;
        public int MaxDepthIndex => _sliceBitmaps.Any() && _sliceBitmaps.First().Any() && _sliceBitmaps.First().First().Any() ?
            _sliceBitmaps.ElementAtOrDefault(ArrayIndex).ElementAtOrDefault(MipIndex).Count - 1 : 0;

        private int _arrayIndex;
        public int ArrayIndex
        {
            get => Math.Min(MaxArrayIndex, _arrayIndex);
            set
            {
                value = Math.Min(value, MaxArrayIndex);
                if(_arrayIndex != value)
                {
                    _arrayIndex = value;
                    OnPropertyChanged(nameof(ArrayIndex));
                    SetSelectedBitmap();
                    SetImageChannels();
                }
            }
        }

        private int _mipIndex;
        public int MipIndex
        {
            get => Math.Min(MaxMipIndex, _mipIndex);
            set
            {
                value = Math.Min(MaxMipIndex, value);
                if(_mipIndex != value)
                {
                    _mipIndex = value;
                    DepthIndex = _depthIndex;
                    OnPropertyChanged(nameof(MipIndex));
                    OnPropertyChanged(nameof(MaxDepthIndex));
                    SetSelectedBitmap();
                    SetImageChannels();
                }
            }
        }

        private int _depthIndex;
        public int DepthIndex
        {
            get => Math.Min(MaxDepthIndex, _depthIndex);
            set
            {
                value = Math.Min(MaxDepthIndex, value);
                if (_depthIndex != value)
                {
                    _depthIndex = value;
                    OnPropertyChanged(nameof(DepthIndex));
                    SetSelectedBitmap();
                    SetImageChannels();
                }
            }
        }

        public BitmapSource SelectedSliceBitmap => _sliceBitmaps.ElementAtOrDefault(ArrayIndex)?.ElementAtOrDefault(MipIndex)?.ElementAtOrDefault(DepthIndex);
        public Slice SelectedSlice => Texture?.Slices?.ElementAtOrDefault(ArrayIndex)?.ElementAtOrDefault(MipIndex)?.ElementAtOrDefault(DepthIndex);
        public long DataSize => Texture?.Slices?.Sum(x => x.Sum(y => y.Sum(z => z.RawContent.LongLength))) ?? 0;

        private void SetSelectedBitmap()
        {
            OnPropertyChanged(nameof(SelectedSliceBitmap));
            OnPropertyChanged(nameof(SelectedSlice));
            OnPropertyChanged(nameof(DataSize));
        }

        private void SetImageChannels()
        {
            OnPropertyChanged(nameof(Channels));
            OnPropertyChanged(nameof(Stride));
        }

        private void OnRegenerateBitmapsCommand(bool isNormalMap)
        {
            GenerateSliceBitmaps(isNormalMap);
            OnPropertyChanged(nameof(SelectedSliceBitmap));
            SetImageChannels();
        }

        private void OnSetChannelCommand(string parameter)
        {
            if(!Keyboard.Modifiers.HasFlag(ModifierKeys.Shift))
            {
                _isRedChannelSelected = false;
                _isGreenChannelSelected = false;
                _isBlueChannelSelected = false;
                _isAlphaChannelSelected = false;
                OnPropertyChanged(nameof(IsRedChannelSelected));
                OnPropertyChanged(nameof(IsGreenChannelSelected));
                OnPropertyChanged(nameof(IsBlueChannelSelected));
                OnPropertyChanged(nameof(IsAlphaChannelSelected));
            }
            
            switch(parameter)
            {
                case "0": IsRedChannelSelected = !IsRedChannelSelected; break;
                case "1": IsGreenChannelSelected = !IsGreenChannelSelected; break;
                case "2": IsBlueChannelSelected = !IsBlueChannelSelected; break;
                case "3": IsAlphaChannelSelected = !IsAlphaChannelSelected; break;
            }
        }

        private void OnSetAllChannelsCommand(object parameter)
        {
            _isRedChannelSelected = true;
            _isGreenChannelSelected = true;
            _isBlueChannelSelected = true;
            _isAlphaChannelSelected = true;
            OnPropertyChanged(nameof(IsRedChannelSelected));
            OnPropertyChanged(nameof(IsGreenChannelSelected));
            OnPropertyChanged(nameof(IsBlueChannelSelected));
            OnPropertyChanged(nameof(IsAlphaChannelSelected));
            SetImageChannels();
        }

        public async void SetAsset(AssetInfo info)
        {
            try
            {
                AssetGuid = info.Guid;
                Texture = null;
                Debug.Assert(info != null && File.Exists(info.FullPath));
                var texture = new Texture();
                State = AssetEditorState.Loading;

                await Task.Run(() =>
                {
                    texture.Load(info.FullPath);
                });

                await SetMipMaps(texture);
                Texture = texture;
            }
            catch(Exception ex)
            {
                Debug.WriteLine(ex.Message);
                Debug.WriteLine($"Failed to set texture for use in textue editor. File : {info.FullPath}");
                Texture = new();
            }
            finally
            {
                State = AssetEditorState.Done;
            }
        }

        private async Task SetMipMaps(Texture texture)
        {
            try
            {
                await Task.Run(() => _slices = texture.ImportSettings.Compress ? ContentToolsAPI.Decompress(texture) : texture.Slices );
                Debug.Assert(_slices?.Any() == true && _slices.First()?.Any() == true);
                GenerateSliceBitmaps(texture.IsNormalMap);
                OnPropertyChanged(nameof(Texture));
                OnPropertyChanged(nameof(DataSize));
            }
            catch(Exception ex)
            {
                Debug.WriteLine(ex.Message);
                Debug.WriteLine($"Failed to load mipmaps from {texture.FileName}");
            }
        }

        private void GenerateSliceBitmaps(bool isNormalMap)
        {
            _sliceBitmaps.Clear();

            foreach(var arraySlice in _slices)
            {
                List<List<BitmapSource>> mipmapsBitmaps = new();
                foreach(var miplevel in arraySlice)
                {
                    List<BitmapSource> sliceBitmap = new();
                    foreach (var slice in miplevel)
                    {
                        var image = BitmapHelper.ImageFromSlice(slice, isNormalMap);
                        Debug.Assert(image != null);
                        sliceBitmap.Add(image);
                    }
                    mipmapsBitmaps.Add(sliceBitmap);
                }
                _sliceBitmaps.Add(mipmapsBitmaps);
            }

            OnPropertyChanged(nameof(MaxMipIndex));
            OnPropertyChanged(nameof(MaxArrayIndex));
            OnPropertyChanged(nameof(MaxDepthIndex));
        }

        private async Task OnReimportCommand(object obj)
        {
            if (Texture == null) return;
            TextureImportSettings settingsBackup = new();
            IAssetImportSettings.CopyImportSettings(Texture.ImportSettings, settingsBackup);
            IAssetImportSettings.CopyImportSettings(ImportSettings, Texture.ImportSettings);

            State = AssetEditorState.Importing;

            bool result = false;
            await Task.Run(() => result = Texture.Import(Texture.FullPath));

            if(result)
            {
                State = AssetEditorState.Loading;
                await SetMipMaps(Texture);
                SetSelectedBitmap();
                SetImageChannels();
                CanSaveChanges = true;
            }
            else
            {
                IAssetImportSettings.CopyImportSettings(settingsBackup, Texture.ImportSettings);
            }

            State = AssetEditorState.Done;
        }

        private async Task OnSaveCommand(object obj)
        {
            if (!CanSaveChanges || Texture == null) return;

            State = AssetEditorState.Saving;
            CanSaveChanges = false;
            await Task.Run(() => Texture.Save(Texture.FullPath));
            State = AssetEditorState.Done;
        }

        public TextureEditor()
        {
            SetAllChannelsCommand = new RelayCommand<string>(OnSetAllChannelsCommand);
            SetChannelCommand = new RelayCommand<string>(OnSetChannelCommand);
            RegenerateBitmapsCommand = new RelayCommand<bool>(OnRegenerateBitmapsCommand);
            ReimportCommand = new RelayCommand<object>(async x => await OnReimportCommand(x));
            SaveCommand = new RelayCommand<object>(async x => await OnSaveCommand(x));
        }
    }
}
