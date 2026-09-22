#version 330

in vec3 fragNormal;
in vec2 fragTexCoord;

uniform sampler2D texture0;

out vec4 finalColor;

void main()
{
    vec3 normal = normalize(fragNormal);

    float brightness;

    if (normal.y > 0.5)
    {
        // Top
        brightness = 1.0;
    }
    else if (normal.y < -0.5)
    {
        // Bottom
        brightness = 0.5;
    }
    else if (normal.z > 0.5)
    {
        // Front
        brightness = 0.8;
    }
    else if (normal.z < -0.5)
    {
        // Back
        brightness = 0.7;
    }
    else if (normal.x > 0.5)
    {
        // Right
        brightness = 0.75;
    }
    else
    {
        // Left
        brightness = 0.65;
    }

    vec4 texColor = texture(texture0, fragTexCoord);

    finalColor = vec4(
        texColor.rgb * brightness,
        texColor.a
    );
}