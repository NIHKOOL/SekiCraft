package dev.sekicraft.mixin;

import com.llamalad7.mixinextras.injector.wrapoperation.Operation;
import com.llamalad7.mixinextras.injector.wrapoperation.WrapOperation;
import dev.sekicraft.combat.SkyrimActorEntity;
import dev.sekicraft.link.Proto;
import dev.sekicraft.link.SkyLink;
import dev.sekicraft.world.SkyClip;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.projectile.arrow.AbstractArrow;
import net.minecraft.world.entity.projectile.arrow.Arrow;
import net.minecraft.world.entity.projectile.arrow.SpectralArrow;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.Vec3;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import net.minecraft.world.level.ClipContext;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.BlockHitResult;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;

/**
 * Arrows and tridents hit Skyrim's exact surfaces. They then stick where they hit: the block state
 * there is air, the same as what they recorded on impact, so vanilla never makes them fall out.
 */
@Mixin(AbstractArrow.class)
public abstract class AbstractArrowMixin {
	@WrapOperation(
		method = "tick",
		at = @At(value = "INVOKE", target = "Lnet/minecraft/world/level/Level;clipIncludingBorder(Lnet/minecraft/world/level/ClipContext;)Lnet/minecraft/world/phys/BlockHitResult;")
	)
	private BlockHitResult sekicraft$hitSkyrim(Level level, ClipContext context, Operation<BlockHitResult> original) {
		return SkyClip.refine(context.getFrom(), context.getTo(), original.call(level, context), SkyClip.Use.PROJECTILE);
	}

	@Unique
	private Vec3 sekicraft$hitAt;

	@Inject(method = "onHitEntity", at = @At("HEAD"))
	private void sekicraft$rememberHit(EntityHitResult hitResult, CallbackInfo ci) {
		this.sekicraft$hitAt = hitResult.getLocation();
		AbstractArrow self = (AbstractArrow) (Object) this;
		if (!self.level().isClientSide() && self.getOwner() instanceof net.minecraft.world.entity.player.Player) {
			dev.sekicraft.SekiCraft.LOG.info("SekiCraft: arrow hit entity {} ({}) at {} speed {}", hitResult.getEntity().getClass().getSimpleName(),
				hitResult.getEntity() instanceof SkyrimActorEntity a ? String.format("stand-in %08X", a.formId()) : "not a stand-in", hitResult.getLocation(),
				String.format("%.2f", self.getDeltaMovement().length()));
		}
	}

	@Inject(method = "onHitBlock", at = @At("HEAD"))
	private void sekicraft$logBlockHit(BlockHitResult hitResult, CallbackInfo ci) {
		AbstractArrow self = (AbstractArrow) (Object) this;
		if (!self.level().isClientSide() && self.getOwner() instanceof net.minecraft.world.entity.player.Player) {
			dev.sekicraft.SekiCraft.LOG.info("SekiCraft: arrow hit {} at {} (shot from {} blocks away)", hitResult instanceof SkyClip.SkyrimHitResult ? "Sekiro geometry" : "a block",
				hitResult.getLocation(), self.getOwner() == null ? "?" : String.format("%.1f", self.getOwner().position().distanceTo(hitResult.getLocation())));
		}
	}

	/**
	 * Where Minecraft counts an arrow as stuck in a creature (it hurt it and didn't pierce): if that
	 * creature is a Skyrim NPC's stand-in, Skyrim pins the arrow to the NPC's skeleton.
	 */
	@WrapOperation(method = "onHitEntity", at = @At(value = "INVOKE", target = "Lnet/minecraft/world/entity/LivingEntity;setArrowCount(I)V"))
	private void sekicraft$stickInSkyrimActor(LivingEntity mob, int count, Operation<Void> original) {
		original.call(mob, count);
		if (!(mob instanceof SkyrimActorEntity actor) || this.sekicraft$hitAt == null || !SkyLink.active()) {
			return;
		}
		AbstractArrow self = (AbstractArrow) (Object) this;
		Vec3 v = self.getDeltaMovement();
		float yaw = (float) (Mth.atan2(v.x, v.z) * Mth.RAD_TO_DEG);
		float pitch = (float) (Mth.atan2(v.y, v.horizontalDistance()) * Mth.RAD_TO_DEG);
		int texture = self instanceof SpectralArrow ? 2 : self instanceof Arrow tippable && tippable.getColor() > 0 ? 1 : 0;
		Vec3 at = this.sekicraft$hitAt;
		SkyLink.pushEvent(Proto.EV_ARROW_STUCK, actor.formId(), (float) at.x, (float) at.y, (float) at.z, yaw, Float.floatToRawIntBits(pitch), texture);
	}
}
