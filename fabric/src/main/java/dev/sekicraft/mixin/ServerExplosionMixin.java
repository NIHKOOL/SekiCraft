package dev.sekicraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.sekicraft.link.Proto;
import dev.sekicraft.link.SkyLink;
import dev.sekicraft.world.SkyDigBlast;
import java.util.List;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.Explosion;
import net.minecraft.world.level.ExplosionDamageCalculator;
import net.minecraft.world.level.ServerExplosion;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.jspecify.annotations.Nullable;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * Minecraft explosions in Skyrim's world: they blow Skyrim's ground and rock apart like blocks
 * (SkyDigBlast), and Skyrim feels them (loose objects are thrown and people knocked away).
 */
@Mixin(ServerExplosion.class)
public abstract class ServerExplosionMixin {
	@Unique
	private @Nullable SkyDigBlast sekicraft$blast;

	@Inject(method = "explode", at = @At("HEAD"))
	private void sekicraft$begin(CallbackInfoReturnable<Integer> cir) {
		this.sekicraft$blast = SkyDigBlast.begin((ServerExplosion) (Object) this);
	}

	@WrapOperation(
		method = "calculateExplodedPositions",
		at = @At(
			value = "INVOKE",
			target = "Lnet/minecraft/world/level/ExplosionDamageCalculator;getBlockExplosionResistance(Lnet/minecraft/world/level/Explosion;Lnet/minecraft/world/level/BlockGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)Ljava/util/Optional;"
		)
	)
	private Optional<Float> sekicraft$skyrimResists(
		ExplosionDamageCalculator calculator, Explosion explosion, BlockGetter level, BlockPos pos, BlockState block, FluidState fluid, Operation<Optional<Float>> original
	) {
		Optional<Float> vanilla = original.call(calculator, explosion, level, pos, block, fluid);
		return this.sekicraft$blast != null ? this.sekicraft$blast.resistance(pos, vanilla) : vanilla;
	}

	@ModifyVariable(method = "explode", at = @At("STORE"), ordinal = 0)
	private List<BlockPos> sekicraft$skyrimBreaks(List<BlockPos> targets) {
		if (this.sekicraft$blast != null) {
			ServerExplosion self = (ServerExplosion) (Object) this;
			this.sekicraft$blast.materialize(targets, self.getBlockInteraction() != Explosion.BlockInteraction.KEEP
				&& self.getBlockInteraction() != Explosion.BlockInteraction.TRIGGER_BLOCK);
		}
		return targets;
	}

	@Inject(method = "explode", at = @At("RETURN"))
	private void sekicraft$tellSkyrim(CallbackInfoReturnable<Integer> cir) {
		if (this.sekicraft$blast != null) {
			this.sekicraft$blast.finish();
			this.sekicraft$blast = null;
		}
		if (!SkyLink.active()) {
			return;
		}
		ServerExplosion self = (ServerExplosion) (Object) this;
		var center = self.center();
		SkyLink.pushEvent(Proto.EV_EXPLOSION, 0, (float) center.x, (float) center.y, (float) center.z, self.radius(), 0);
	}
}
