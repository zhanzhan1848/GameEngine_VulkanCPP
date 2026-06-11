using System.Text.Json.Serialization;
using PrimalEditor_Avalonia.ViewModels;

namespace PrimalEditor_Avalonia.Components;

public class Transform : Component
{
    private float _posX;
    private float _posY;
    private float _posZ;
    private float _rotX;
    private float _rotY;
    private float _rotZ;
    private float _scaleX = 1.0f;
    private float _scaleY = 1.0f;
    private float _scaleZ = 1.0f;

    [JsonPropertyName("PositionX")]
    public float PositionX
    {
        get => _posX;
        set
        {
            if (_posX != value)
            {
                _posX = value;
                OnPropertyChanged(nameof(PositionX));
            }
        }
    }

    [JsonPropertyName("PositionY")]
    public float PositionY
    {
        get => _posY;
        set
        {
            if (_posY != value)
            {
                _posY = value;
                OnPropertyChanged(nameof(PositionY));
            }
        }
    }

    [JsonPropertyName("PositionZ")]
    public float PositionZ
    {
        get => _posZ;
        set
        {
            if (_posZ != value)
            {
                _posZ = value;
                OnPropertyChanged(nameof(PositionZ));
            }
        }
    }

    [JsonPropertyName("RotationX")]
    public float RotationX
    {
        get => _rotX;
        set
        {
            if (_rotX != value)
            {
                _rotX = value;
                OnPropertyChanged(nameof(RotationX));
            }
        }
    }

    [JsonPropertyName("RotationY")]
    public float RotationY
    {
        get => _rotY;
        set
        {
            if (_rotY != value)
            {
                _rotY = value;
                OnPropertyChanged(nameof(RotationY));
            }
        }
    }

    [JsonPropertyName("RotationZ")]
    public float RotationZ
    {
        get => _rotZ;
        set
        {
            if (_rotZ != value)
            {
                _rotZ = value;
                OnPropertyChanged(nameof(RotationZ));
            }
        }
    }

    [JsonPropertyName("ScaleX")]
    public float ScaleX
    {
        get => _scaleX;
        set
        {
            if (_scaleX != value)
            {
                _scaleX = value;
                OnPropertyChanged(nameof(ScaleX));
            }
        }
    }

    [JsonPropertyName("ScaleY")]
    public float ScaleY
    {
        get => _scaleY;
        set
        {
            if (_scaleY != value)
            {
                _scaleY = value;
                OnPropertyChanged(nameof(ScaleY));
            }
        }
    }

    [JsonPropertyName("ScaleZ")]
    public float ScaleZ
    {
        get => _scaleZ;
        set
        {
            if (_scaleZ != value)
            {
                _scaleZ = value;
                OnPropertyChanged(nameof(ScaleZ));
            }
        }
    }
}