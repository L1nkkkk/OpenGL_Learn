bool LoadWorldPosition(vec2 uv, out vec3 worldPosition)
{
	if (!reconstructPosition) {
		vec4 positionDepth = texture(gPosition, uv);
		if (positionDepth.a <= 0.0) return false;
		worldPosition = positionDepth.xyz;
		return true;
	}

	// OpenGL 3.3 window depth maps NDC z from [-1, 1] to [0, 1]. A clear
	// depth of 1.0 is the candidate's explicit background/invalid marker.
	float deviceDepth = texture(gDepth, uv).r;
	if (deviceDepth >= 1.0) return false;
	vec4 clip = vec4(
		uv * 2.0 - 1.0,
		deviceDepth * 2.0 - 1.0,
		1.0);
	vec4 viewPosition = inverseProjection * clip;
	viewPosition /= viewPosition.w;
	worldPosition = (inverseView * viewPosition).xyz;
	return true;
}
