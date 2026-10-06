package dev.sekicraft.client.mixin;

import dev.sekicraft.client.SkyClient;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
public abstract class MinecraftMixin {
	@Inject(method = "runTick", at = @At("HEAD"))
	private void sekicraft$beginFrame(boolean advanceGameTime, CallbackInfo ci) {
		SkyClient.beginFrame();
	}

	@Inject(
		method = "renderFrame",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/client/renderer/GameRenderer;render()V", shift = At.Shift.AFTER)
	)
	private void sekicraft$afterRender(boolean advanceGameTime, CallbackInfo ci) {
		SkyClient.afterRender();
	}

	@Inject(method = "renderFrame", at = @At("TAIL"))
	private void sekicraft$pace(boolean advanceGameTime, CallbackInfo ci) {
		SkyClient.paceFrame();
	}
}
