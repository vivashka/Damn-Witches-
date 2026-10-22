#include "Enemy/Antsybal.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

namespace
{
// Что за поверхность у грани — от этого зависит покраска (на глаз не ляжет грязь, на копыто — мох).
enum class ESurf : uint8 { Skin, Fur, Horn, Hoof, Eye, Algae, Moss };

FLinearColor Rgb(float Red, float Green, float Blue, float Glow = 0.f) { return FLinearColor(Red, Green, Blue, Glow); }
float RandIn(FRandomStream& R, const FFloatInterval& I) { return R.FRandRange(I.Min, I.Max); }

// Сборщик плоско затенённой low-poly геометрии: у каждого треугольника свои вершины и нормаль.
struct FLowPoly
{
	FRandomStream* R = nullptr;
	ESurf Surf = ESurf::Skin;
	TArray<FVector> V;
	TArray<int32> T;
	TArray<FVector> N;
	TArray<FLinearColor> C;
	TArray<ESurf> FaceSurf; // по грани; вершины грани F — 3F, 3F+1, 3F+2

	// Inside — точка внутри детали: грань разворачивается лицом от неё.
	void Tri(FVector A, FVector B, FVector Cc, const FVector& Inside, FLinearColor Color)
	{
		// Лицевая у UE та грань, для которой (C−A)×(B−A) смотрит наружу (см. UKismetProceduralMeshLibrary::GenerateBoxMesh).
		FVector Normal = (Cc - A) ^ (B - A);
		const double Len = Normal.Size();
		if (Len < 1e-4) return;
		Normal /= Len;
		if ((Normal | ((A + B + Cc) / 3.0 - Inside)) < 0.0) { Swap(B, Cc); Normal = -Normal; }
		// Разброс тона по граням — так читается лепка low-poly.
		const float Shade = 1.f + R->FRandRange(-0.09f, 0.09f);
		Color = FLinearColor(Color.R * Shade, Color.G * Shade, Color.B * Shade, Color.A);
		for (const FVector& P : { A, B, Cc })
		{
			T.Add(V.Add(P));
			N.Add(Normal);
			C.Add(Color);
		}
		FaceSurf.Add(Surf);
	}

	// Трубка по ломаной; в каждом узле — эллипс сечения: X — радиус по «верху» сечения, Y — вбок. Радиус 0 — остриё.
	void Tube(const TArray<FVector>& P, const TArray<FVector2D>& Rad, int32 Sides, FLinearColor From, FLinearColor To, float Jitter = 0.f)
	{
		const int32 Num = P.Num();
		if (Num < 2 || Rad.Num() != Num) return;
		TArray<TArray<FVector>> Rings;
		Rings.SetNum(Num);
		FVector U = FVector::ZeroVector;
		for (int32 I = 0; I < Num; I++)
		{
			const FVector Axis = (P[FMath::Min(I + 1, Num - 1)] - P[FMath::Max(I - 1, 0)]).GetSafeNormal();
			// «Верх» сечения переносится от узла к узлу — без перекрутов на изогнутых рогах и хвосте.
			const FVector Ref = I > 0 ? U : (FMath::Abs(Axis.Z) < 0.9 ? FVector::UpVector : FVector::ForwardVector);
			U = (Ref - Axis * (Ref | Axis)).GetSafeNormal();
			const FVector W = Axis ^ U;
			Rings[I].SetNum(Sides);
			for (int32 K = 0; K < Sides; K++)
			{
				const float A = 2.f * PI * K / Sides;
				const float J = Jitter > 0.f ? 1.f + R->FRandRange(-Jitter, Jitter) : 1.f;
				Rings[I][K] = P[I] + (U * FMath::Cos(A) * Rad[I].X + W * FMath::Sin(A) * Rad[I].Y) * J;
			}
		}
		for (int32 I = 0; I + 1 < Num; I++)
		{
			const FVector Inside = (P[I] + P[I + 1]) * 0.5;
			const FLinearColor Col = FMath::Lerp(From, To, (I + 0.5f) / (Num - 1));
			for (int32 K = 0; K < Sides; K++)
			{
				const int32 K1 = (K + 1) % Sides;
				Tri(Rings[I][K], Rings[I][K1], Rings[I + 1][K1], Inside, Col);
				Tri(Rings[I][K], Rings[I + 1][K1], Rings[I + 1][K], Inside, Col);
			}
		}
		if (Rad[0].GetMax() > 0.0) Cap(Rings[0], P[0], P[1], From);
		if (Rad.Last().GetMax() > 0.0) Cap(Rings.Last(), P.Last(), P[Num - 2], To);
	}

	void Cap(const TArray<FVector>& Ring, const FVector& Center, const FVector& Inside, FLinearColor Col)
	{
		for (int32 K = 0; K < Ring.Num(); K++) Tri(Center, Ring[K], Ring[(K + 1) % Ring.Num()], Inside, Col);
	}

	void Spike(const FVector& Base, const FVector& Tip, float Radius, int32 Sides, FLinearColor From, FLinearColor To)
	{
		Tube({ Base, Tip }, { FVector2D(Radius), FVector2D(0.0) }, Sides, From, To);
	}

	// Низкополигональный эллипсоид; соседние пояса сдвинуты на полшага, Lumpy — неровность поверхности.
	void Blob(const FVector& Center, const FVector& Radii, const FQuat& Rot, int32 Lon, int32 Lat, float Lumpy, FLinearColor Col)
	{
		auto At = [&](float Theta, float Phi)
		{
			const FVector Unit(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
			const float J = Lumpy > 0.f ? 1.f + R->FRandRange(-Lumpy, Lumpy) : 1.f;
			return Center + Rot.RotateVector(Unit * Radii * J);
		};
		const FVector Top = At(0.f, 0.f), Bottom = At(PI, 0.f);
		TArray<TArray<FVector>> Rings;
		Rings.SetNum(Lat - 1);
		for (int32 I = 1; I < Lat; I++)
		{
			Rings[I - 1].SetNum(Lon);
			for (int32 K = 0; K < Lon; K++) Rings[I - 1][K] = At(PI * I / Lat, 2.f * PI * (K + 0.5f * (I & 1)) / Lon);
		}
		for (int32 K = 0; K < Lon; K++)
		{
			const int32 K1 = (K + 1) % Lon;
			Tri(Top, Rings[0][K], Rings[0][K1], Center, Col);
			Tri(Bottom, Rings.Last()[K1], Rings.Last()[K], Center, Col);
			for (int32 I = 0; I + 1 < Rings.Num(); I++)
			{
				Tri(Rings[I][K], Rings[I][K1], Rings[I + 1][K1], Center, Col);
				Tri(Rings[I][K], Rings[I + 1][K1], Rings[I + 1][K], Center, Col);
			}
		}
	}
};

// Анцыбал в сантиметрах: копыта на z = 0, морда к +X, +Y — правый бок. Цвета линейные; альфа — свечение.
struct FAntsybalBuilder
{
	FLowPoly M;

	// ---- Пропорции основы ----
	static constexpr float Bulk = 1.05f, Belly = 1.1f, Hump = 1.f, Arm = 1.1f, H = 1.f, EyeSize = 1.1f, EarLen = 11.f, TailLen = 75.f;
	const float Hunch = FMath::DegreesToRadians(28.f);

	// ---- Окрас основы ----
	const FLinearColor Skin = Rgb(0.065f, 0.07f, 0.04f);
	const FLinearColor SkinDark = Skin * 0.6f;
	const FLinearColor BellyCol = FMath::Lerp(Skin * 1.5f, Rgb(0.12f, 0.11f, 0.06f), 0.4f);
	const FLinearColor Fur = Rgb(0.035f, 0.026f, 0.015f);
	const FLinearColor HornBase = Rgb(0.3f, 0.26f, 0.17f), HornTip = Rgb(0.03f, 0.025f, 0.02f);
	const FLinearColor Hoof = Rgb(0.012f, 0.01f, 0.008f), Claw = Rgb(0.06f, 0.05f, 0.035f);
	const FLinearColor Eye = Rgb(0.8f, 0.45f, 0.02f, 1.f), Pupil = Rgb(0.004f, 0.004f, 0.003f);
	const FLinearColor Algae = Rgb(0.035f, 0.11f, 0.012f), AlgaeTip = Rgb(0.09f, 0.13f, 0.025f);
	const FLinearColor MossCol = Rgb(0.03f, 0.085f, 0.012f), MossLight = Rgb(0.075f, 0.13f, 0.02f);
	const FLinearColor MudWet = Rgb(0.03f, 0.022f, 0.013f), MudDry = Rgb(0.075f, 0.06f, 0.038f);

	// ---- Точки крепления, которые основа отдаёт навесному слою ----
	const FVector Down = FVector(0, 0, -1);
	const FVector Pelvis = FVector(0, 0, 94);
	FVector Spine, Back, ShoulderBase, HeadC, HumpTop;
	FVector Shoulder[2], Elbow[2], Wrist[2], Hip[2], Knee[2], Hock[2], Fet[2];
	FVector TailEnd, TailDir;

	FVector HeadP(float X, float Y, float Z) const { return HeadC + FVector(X, Y, Z) * H; }
	static int32 Side(int32 S) { return S > 0 ? 1 : 0; }

	// 1. Основа — одна на весь вид.
	void BuildBase()
	{
		Spine = FVector(FMath::Sin(Hunch), 0, FMath::Cos(Hunch)); // ось туловища
		Back = FVector(-FMath::Cos(Hunch), 0, FMath::Sin(Hunch)); // из спины наружу
		const FQuat Tilt = FRotator(-FMath::RadiansToDegrees(Hunch), 0, 0).Quaternion();
		ShoulderBase = Pelvis + Spine * 55;
		HeadC = ShoulderBase + FVector(18, 0, 8); // голова вынесена вперёд
		HumpTop = Pelvis + Spine * 48 + Back * (8 + 13 * Hump);

		// Туловище: сутулое, с горбом и брюхом.
		M.Surf = ESurf::Skin;
		M.Blob(Pelvis, FVector(15, 19 * Bulk, 14), Tilt, 7, 5, 0.08f, Skin);
		M.Blob(Pelvis + Spine * 20 + FVector(6, 0, 0), FVector(16 * Bulk * Belly, 18 * Bulk, 17), Tilt, 7, 5, 0.06f, BellyCol);
		M.Blob(Pelvis + Spine * 42, FVector(17 * Bulk, 23 * Bulk, 20), Tilt, 8, 5, 0.08f, Skin);
		M.Blob(Pelvis + Spine * 48 + Back * 8, FVector(13, 18 * Bulk, 14) * Hump, Tilt, 7, 4, 0.12f, SkinDark);
		M.Tube({ ShoulderBase - Spine * 6, ShoulderBase + FVector(9, 0, 6), HeadC + FVector(-6, 0, -4) },
			{ FVector2D(12 * Bulk, 14 * Bulk), FVector2D(9, 10), FVector2D(8, 8) }, 6, Skin, Skin, 0.08f);

		// Голова: лягушачья морда, глаза навыкате, висячие козьи уши.
		M.Blob(HeadC, FVector(13, 11, 11) * H, FQuat::Identity, 7, 5, 0.07f, Skin);
		M.Tube({ HeadP(5, 0, -3), HeadP(14, 0, -6), HeadP(22, 0, -9) },
			{ FVector2D(8, 9) * H, FVector2D(6.5f, 7.5f) * H, FVector2D(4.5f, 5.5f) * H }, 6, Skin, SkinDark, 0.06f);
		M.Blob(HeadP(22, 0, -8), FVector(3, 4.5f, 3) * H, FQuat::Identity, 5, 3, 0.1f, SkinDark * 0.6f); // нос
		// Нижняя челюсть — со щелью рта под мордой.
		M.Tube({ HeadP(3, 0, -10), HeadP(12, 0, -13), HeadP(19, 0, -13.5f) },
			{ FVector2D(4, 7.5f) * H, FVector2D(3.2f, 6.5f) * H, FVector2D(2.5f, 4.5f) * H }, 6, SkinDark, SkinDark, 0.06f);
		for (int32 S : { -1, 1 })
		{
			const FVector E = HeadP(9, S * 7.5f, 5);
			const float Er = 4.5f * EyeSize * H;
			M.Surf = ESurf::Skin;
			M.Blob(HeadP(7, S * 7, 7.5f), FVector(5.5f, 5, 2.5f) * H, FRotator(0, 0, -15.f * S).Quaternion(), 5, 3, 0.1f, SkinDark); // надбровье
			M.Tube({ HeadP(-3, S * 9, 3), HeadP(-4, S * (9 + EarLen * 0.55f), 0), HeadP(-6, S * (9 + EarLen), -5) },
				{ FVector2D(1.3f, 4.2f) * H, FVector2D(1.0f, 3.6f) * H, FVector2D(0.0) }, 5, Skin, SkinDark);
			M.Surf = ESurf::Eye;
			M.Blob(E, FVector(Er), FQuat::Identity, 6, 4, 0.04f, Eye);
			const FVector Out = FVector(1, S * 0.55f, 0.15f).GetSafeNormal(); // зрачок — козья щель
			M.Blob(E + Out * Er * 0.88f, FVector(0.7f, 2.6f, 1.0f) * EyeSize * H, FRotationMatrix::MakeFromX(Out).ToQuat(), 4, 3, 0.f, Pupil);
			M.Surf = ESurf::Horn; // клыки из-под губы
			M.Spike(HeadP(18, S * 4.8f, -12.5f), HeadP(19.5f, S * 6.5f, -4), 1.1f * H, 4, HornBase, HornBase * 1.3f);
		}

		// Руки: длинные, до колен, с когтями.
		for (int32 S : { -1, 1 })
		{
			const int32 I = Side(S);
			Shoulder[I] = ShoulderBase + FVector(0, S * 22 * Bulk, -6);
			Elbow[I] = Shoulder[I] + FVector(4, S * 6, -30 * Arm);
			Wrist[I] = Elbow[I] + FVector(14, 0, -28 * Arm);
			M.Surf = ESurf::Skin;
			M.Blob(Shoulder[I], FVector(9 * Bulk), FQuat::Identity, 6, 4, 0.1f, Skin);
			M.Tube({ Shoulder[I], Elbow[I] }, { FVector2D(7.5f * Bulk), FVector2D(5.5f) }, 6, Skin, Skin, 0.08f);
			M.Blob(Elbow[I], FVector(5.8f), FQuat::Identity, 5, 3, 0.1f, Skin);
			M.Tube({ Elbow[I], Wrist[I] }, { FVector2D(6.0f), FVector2D(4.0f) }, 6, Skin, SkinDark, 0.08f);
			const FVector Palm = Wrist[I] + FVector(3, 0, -6);
			M.Blob(Palm, FVector(5.5f, 4, 6.5f), FQuat::Identity, 5, 3, 0.1f, SkinDark);
			for (int32 F = 0; F < 4; F++) // три длинных пальца и большой
			{
				const bool bThumb = F == 3;
				const FVector Base = Palm + (bThumb ? FVector(3, -S * 4, -1) : FVector(3, S * (F - 1) * 3.f, -5));
				const FVector Bend = Base + (bThumb ? FVector(5, -S * 3, -5) : FVector(3, S * (F - 1) * 1.5f, -8 * Arm));
				const FVector Tip = Bend + (bThumb ? FVector(4, -S, -3) : FVector(4, 0, -6 * Arm));
				M.Surf = ESurf::Skin;
				M.Tube({ Base, Bend, Tip }, { FVector2D(1.8f), FVector2D(1.5f), FVector2D(1.2f) }, 4, SkinDark, SkinDark);
				M.Surf = ESurf::Horn;
				M.Spike(Tip, Tip + ((Tip - Bend).GetSafeNormal() + FVector(0.6f, 0, 0)).GetSafeNormal() * 4.5f, 1.2f, 4, Claw, Claw * 0.5f);
			}
		}

		// Ноги: козьи — коленом вперёд, скакательным суставом назад, на копытах.
		for (int32 S : { -1, 1 })
		{
			const int32 I = Side(S);
			Hip[I] = Pelvis + FVector(-2, S * 12 * Bulk, -4);
			Knee[I] = FVector(16, S * 15 * Bulk, 56);
			Hock[I] = FVector(-10, S * 14, 26);
			Fet[I] = FVector(-3, S * 13, 8);
			M.Surf = ESurf::Fur;
			M.Tube({ Hip[I], Knee[I] }, { FVector2D(12 * Bulk, 11 * Bulk), FVector2D(7.0f) }, 6, Fur, Fur, 0.1f);
			M.Blob(Knee[I], FVector(7), FQuat::Identity, 5, 3, 0.1f, Fur);
			M.Tube({ Knee[I], Hock[I] }, { FVector2D(6.5f), FVector2D(4.0f) }, 6, Fur, Fur, 0.08f);
			M.Surf = ESurf::Skin;
			M.Tube({ Hock[I], Fet[I] }, { FVector2D(4.0f), FVector2D(3.2f) }, 5, SkinDark, SkinDark, 0.05f);
			M.Surf = ESurf::Hoof;
			M.Blob(Fet[I] + FVector(3, 0, -4.5f), FVector(6, 5, 3.8f), FQuat::Identity, 6, 3, 0.05f, Hoof);
		}

		// Хвост.
		const int32 Steps = 8;
		TArray<FVector> P{ Pelvis + Back * 12 + FVector(0, 0, -4) };
		TArray<FVector2D> Rad{ FVector2D(2.6f) };
		for (int32 I = 1; I <= Steps; I++)
		{
			const float T = float(I) / Steps;
			const FVector D(-1, 0.25f * FMath::Sin(T * PI * 1.5f), FMath::Lerp(-1.1f, 0.6f, T * T));
			P.Add(P.Last() + D.GetSafeNormal() * TailLen / Steps);
			Rad.Add(FVector2D(FMath::Lerp(2.6f, 1.1f, T)));
		}
		M.Surf = ESurf::Skin;
		M.Tube(P, Rad, 4, Skin, SkinDark, 0.05f);
		TailEnd = P.Last();
		TailDir = (P.Last() - P[Steps - 1]).GetSafeNormal();
	}

	// ---- Навесные детали ----

	void Horn(const FVector& Base, const FVector& Dir0, const FVector& Toward, const FVector& Outward, float Len, float Curl, float Thick, bool bBroken)
	{
		const int32 Steps = 7;
		const FVector Dir = Dir0.GetSafeNormal();
		const FVector Perp = (Toward - Dir * (Toward | Dir)).GetSafeNormal();
		TArray<FVector> P{ Base };
		TArray<FVector2D> Rad{ FVector2D(Thick) };
		for (int32 I = 1, Last = bBroken ? Steps / 2 + 1 : Steps; I <= Last; I++)
		{
			const float T = float(I) / Steps;
			// Рог загибается от Dir к Toward и уходит вбок — завиток, как у барана.
			const FVector D = Dir * FMath::Cos(Curl * T) + Perp * FMath::Sin(Curl * T) + Outward * 0.35f;
			P.Add(P.Last() + D.GetSafeNormal() * Len / Steps);
			Rad.Add(FVector2D(Thick * FMath::Pow(1.f - T, 0.8f) * ((I & 1) ? 1.08f : 0.94f))); // кольца на роге
		}
		if (bBroken) Rad.Last() = FVector2D(Thick * 0.55f); // обломок — с торцом
		M.Surf = ESurf::Horn;
		M.Tube(P, Rad, 5, HornBase, bBroken ? FMath::Lerp(HornBase, HornTip, 0.4f) : HornTip, 0.05f);
	}

	// Свисающая прядь: тина или волосы.
	void Strand(const FVector& Root, float Len, float Thick, const FVector& Sway, FLinearColor From, FLinearColor To)
	{
		const int32 Steps = 4;
		TArray<FVector> P{ Root };
		TArray<FVector2D> Rad{ FVector2D(Thick) };
		for (int32 I = 1; I <= Steps; I++)
		{
			const float T = float(I) / Steps;
			const FVector D = Down + Sway * FMath::Sin(T * PI) + FVector(M.R->FRandRange(-0.3f, 0.3f), M.R->FRandRange(-0.3f, 0.3f), 0);
			P.Add(P.Last() + D.GetSafeNormal() * Len / Steps);
			Rad.Add(FVector2D(Thick * (1.f - T)));
		}
		M.Surf = ESurf::Algae;
		M.Tube(P, Rad, 3, From, To);
	}

	// Кочка мха, лежащая на поверхности с нормалью Normal, с торчащими стебельками.
	void MossClump(const FVector& At, const FVector& Normal, float Size)
	{
		FRandomStream& R = *M.R;
		const FQuat Rot = FRotationMatrix::MakeFromZ(Normal).ToQuat() * FQuat(FVector::UpVector, R.FRandRange(0.f, 2.f * PI));
		M.Surf = ESurf::Moss;
		M.Blob(At, FVector(Size * R.FRandRange(0.8f, 1.2f), Size * R.FRandRange(0.8f, 1.2f), Size * 0.4f), Rot, 6, 3, 0.3f, MossCol);
		for (int32 I = 0, Num = R.RandRange(1, 4); I < Num; I++)
		{
			const FVector Base = At + Rot.RotateVector(FVector(R.FRandRange(-0.5f, 0.5f), R.FRandRange(-0.5f, 0.5f), 0.2f) * Size);
			M.Spike(Base, Base + (Normal + R.VRand() * 0.5f).GetSafeNormal() * R.FRandRange(3.f, 6.f), 1.f, 3, MossLight, MossLight * 1.2f);
		}
	}

	// 2. Навесное — по точкам крепления основы, из сида.
	void AddDetails(const FAntsybalLook& Look)
	{
		FRandomStream& R = *M.R;
		const float Density = RandIn(R, Look.FurDensity), FurLen = RandIn(R, Look.FurLength);
		auto Tufts = [&](int32 Base) { return FMath::Max(0, FMath::RoundToInt(Base * Density * R.FRandRange(0.85f, 1.15f))); };

		// Рога.
		const float HornLen = 30.f * RandIn(R, Look.HornLength);
		const float HornCurl = FMath::DegreesToRadians(R.FRandRange(130.f, 170.f));
		const int32 Broken = R.FRand() < Look.BrokenHornChance ? (R.RandBool() ? 1 : -1) : 0;
		for (int32 S : { -1, 1 })
			Horn(HeadP(-3, S * 5, 9), FVector(-0.15f, S * 0.35f, 1), FVector(-1, S * 0.3f, -0.2f), FVector(0, S, 0),
				HornLen * H * R.FRandRange(0.95f, 1.05f), HornCurl, 3.4f * H, Broken == S);

		// Космы на бёдрах — вниз и наружу, не в другую ногу.
		for (int32 S : { -1, 1 })
		{
			const int32 I = Side(S);
			const FVector Thigh = (Knee[I] - Hip[I]).GetSafeNormal();
			const FVector X1 = (Thigh ^ FVector(0, 1, 0)).GetSafeNormal(), Y1 = Thigh ^ X1;
			for (int32 K = 0, Num = Tufts(14); K < Num; K++)
			{
				const float T = R.FRandRange(0.1f, 0.95f), A = R.FRandRange(0.f, 2.f * PI);
				const FVector Radial = X1 * FMath::Cos(A) + Y1 * FMath::Sin(A);
				if (Radial.Y * S < -0.6f) continue;
				const FVector Base = FMath::Lerp(Hip[I], Knee[I], T) + Radial * FMath::Lerp(11.5f * Bulk, 7.f, T) * 0.8f;
				const FVector Dir = (Radial * 0.7f + Down * 0.9f + FVector(-0.3f, 0, 0) + R.VRand() * 0.2f).GetSafeNormal();
				M.Surf = ESurf::Fur;
				M.Spike(Base, Base + Dir * R.FRandRange(7.f, 13.f) * FurLen, R.FRandRange(2.5f, 3.8f), 4, Fur * 1.2f, Fur * 0.6f);
			}
			for (int32 K = 0, Num = Tufts(3); K < Num; K++) // бахрома за скакательным суставом
			{
				const FVector Base = Hock[I] + FVector(-2, S * R.FRandRange(-2.f, 2.f), R.FRandRange(0.f, 8.f));
				M.Spike(Base, Base + FVector(-8 * FurLen, S * 2, R.FRandRange(-6.f, -2.f)), 2.2f, 4, Fur, Fur * 0.6f);
			}
			for (int32 K = 0; K < 5; K++) // щётка над копытом
			{
				const float A = 2.f * PI * (K + R.FRandRange(-0.2f, 0.2f)) / 5;
				const FVector Radial(FMath::Cos(A), FMath::Sin(A), 0);
				M.Spike(Fet[I] + Radial * 2.5f + FVector(0, 0, 3), Fet[I] + Radial * 5.5f + FVector(0, 0, -3 * FurLen), 2.f, 4, Fur, Fur * 0.6f);
			}
			for (int32 K = 0, Num = Tufts(3); K < Num; K++) // шерсть на предплечье
			{
				const FVector P = FMath::Lerp(Elbow[I], Wrist[I], R.FRandRange(0.2f, 0.8f)) + FVector(-4, S * 2, 0);
				M.Spike(P, P + FVector(-7 * FurLen, S * 2, R.FRandRange(-5.f, -1.f)), 2.5f, 4, Fur, Fur * 0.7f);
			}
		}
		// Шерстяная «юбка» по тазу.
		for (int32 K = 0, Num = Tufts(12); K < Num; K++)
		{
			const float A = 2.f * PI * (K + R.FRandRange(-0.3f, 0.3f)) / Num;
			const FVector Radial(FMath::Cos(A), FMath::Sin(A), 0);
			const FVector Base = Pelvis + FVector(Radial.X * 13, Radial.Y * 17 * Bulk, -6);
			M.Spike(Base, Base + (Radial * 0.5f + Down).GetSafeNormal() * R.FRandRange(8.f, 14.f) * FurLen, 3.f, 4, Fur * 1.2f, Fur * 0.6f);
		}
		// Щетина по хребту.
		for (int32 K = 0, Num = Tufts(8); K < Num; K++)
		{
			const float T = (K + 0.5f) / Num;
			const float HumpBulge = (8 + 12 * Hump) * (1 - FMath::Square((T - 0.85f) / 0.35f));
			const FVector Base = Pelvis + Spine * (T * 55) + Back * (FMath::Max(FMath::Lerp(14.f, 16.f * Bulk, T), HumpBulge) - 2);
			M.Spike(Base, Base + (Back + Spine * 0.4f + R.VRand() * 0.25f).GetSafeNormal() * R.FRandRange(6.f, 12.f) * FurLen, 2.5f, 4, Fur, Fur * 0.6f);
		}
		// Кисточка хвоста.
		for (int32 K = 0, Num = FMath::Max(3, Tufts(5)); K < Num; K++)
			M.Spike(TailEnd - TailDir * 2, TailEnd + (TailDir + R.VRand() * 0.6f).GetSafeNormal() * R.FRandRange(7.f, 11.f) * FurLen, 2.2f, 4, Fur, Fur * 0.6f);

		// Тина: волосы с затылка на спину, борода, пряди с плеч и рогов.
		const FLinearColor Hair = FMath::Lerp(Algae, Fur, R.FRandRange(0.3f, 0.7f));
		if (R.FRand() < Look.HairChance)
		{
			const float Len = R.FRandRange(15.f, 35.f);
			for (int32 K = 0, Num = R.RandRange(5, 9); K < Num; K++)
				Strand(HeadP(R.FRandRange(-9.f, -3.f), R.FRandRange(-8.f, 8.f), R.FRandRange(2.f, 8.f)), Len * R.FRandRange(0.7f, 1.2f), 2.2f,
					FVector(-0.8f, 0, 0), Hair, AlgaeTip);
		}
		if (R.FRand() < Look.BeardChance)
		{
			const float Len = R.FRandRange(10.f, 35.f);
			for (int32 K = 0, Num = R.RandRange(4, 7); K < Num; K++)
			{
				const float Y = R.FRandRange(-4.5f, 4.5f);
				Strand(HeadP(R.FRandRange(9.f, 17.f), Y, -14), Len * R.FRandRange(0.6f, 1.2f), R.FRandRange(1.6f, 2.6f),
					FVector(R.FRandRange(-0.2f, 0.4f), Y * 0.05f, 0), Algae, AlgaeTip);
			}
		}
		for (int32 S : { -1, 1 })
		{
			if (R.FRand() < Look.AlgaeChance)
				for (int32 K = 0, Num = R.RandRange(1, 3); K < Num; K++)
					Strand(Shoulder[Side(S)] + FVector(R.FRandRange(-6.f, 6.f), S * R.FRandRange(-4.f, 4.f), 7), R.FRandRange(15.f, 30.f), 2.f,
						FVector(0, S * 0.3f, 0), Algae, AlgaeTip);
			if (R.FRand() < Look.AlgaeChance * 0.5f && Broken != S) // тина, намотанная на рог
				Strand(HeadP(-10, S * 12, 18), R.FRandRange(10.f, 22.f), 1.6f, FVector(0, S * 0.2f, 0), Algae, AlgaeTip);
		}

		// Бородавки на загривке.
		for (int32 K = 0, Num = R.RandRange(0, Look.MaxWarts); K < Num; K++)
		{
			M.Surf = ESurf::Skin;
			M.Blob(Pelvis + Spine * R.FRandRange(30.f, 55.f) + Back * R.FRandRange(14.f, 19.f) * Bulk + FVector(0, R.FRandRange(-15.f, 15.f) * Bulk, 0),
				FVector(R.FRandRange(2.f, 3.5f)), FQuat::Identity, 4, 3, 0.15f, FMath::Lerp(SkinDark, Algae, 0.3f));
		}
	}

	// 3. Поверхность — покраска граней: оттенок кожи, грязь снизу, мох пятнами сверху, мокрота.
	void Paint(const FAntsybalLook& Look, float Moss, TArray<FVector2D>& Wet)
	{
		FRandomStream& R = *M.R;
		const float Mud = RandIn(R, Look.Mud), MudLine = RandIn(R, Look.MudLine), Wetness = RandIn(R, Look.Wetness);
		const FVector Offset = R.VRand() * 100.f; // своё место в шуме — свои пятна
		const FLinearColor Tones[] = { Rgb(0.08f, 0.06f, 0.035f), Rgb(0.05f, 0.07f, 0.065f), Rgb(0.09f, 0.085f, 0.04f) };
		const FLinearColor Tone = Tones[R.RandHelper(3)];
		const float TintAmount = R.FRandRange(0.f, Look.SkinTint);
		const FLinearColor Tint(FMath::Lerp(1.f, Tone.R / Skin.R, TintAmount), FMath::Lerp(1.f, Tone.G / Skin.G, TintAmount), FMath::Lerp(1.f, Tone.B / Skin.B, TintAmount), 1.f);

		Wet.SetNumUninitialized(M.V.Num());
		for (int32 F = 0; F < M.FaceSurf.Num(); F++)
		{
			const ESurf S = M.FaceSurf[F];
			const FVector Ctr = (M.V[3 * F] + M.V[3 * F + 1] + M.V[3 * F + 2]) / 3.0;
			const FVector& Nrm = M.N[3 * F];
			FLinearColor Col = M.C[3 * F];
			float W = Wetness * 0.6f;
			if (S == ESurf::Skin) Col *= Tint;
			if (S == ESurf::Eye) W = 1.f; // глаза всегда блестят
			else
			{
				// Грязь: до неровной линии — сплошь, выше — брызгами.
				const float Line = MudLine + 15.f * FMath::PerlinNoise3D(Ctr * 0.04f + Offset);
				float MudMask = FMath::Clamp((Line - Ctr.Z) / 12.f, 0.f, 1.f);
				if (MudMask < 0.5f && Ctr.Z < Line + 40.f && FMath::PerlinNoise3D(Ctr * 0.15f + Offset * 1.7f) > 0.35f) MudMask = 0.6f;
				MudMask *= Mud;
				if (MudMask > 0.05f)
				{
					const FLinearColor MudCol = FMath::Lerp(MudWet, MudDry, 0.5f + 0.5f * FMath::PerlinNoise3D(Ctr * 0.1f - Offset)) * R.FRandRange(0.9f, 1.1f);
					Col = FMath::Lerp(Col, MudCol, FMath::Min(MudMask, 0.9f));
					W = FMath::Max(W, MudMask * Wetness);
				}
				// Мох: на гранях, глядящих вверх, пятнами — грань целиком, как положено low-poly.
				if (S != ESurf::Hoof && S != ESurf::Moss && S != ESurf::Algae && Ctr.Z > Line)
				{
					const float Up = FMath::Clamp((Nrm.Z - 0.15f) / 0.6f, 0.f, 1.f);
					const float Patch = 0.5f + 0.5f * FMath::PerlinNoise3D(Ctr * 0.06f + Offset * 0.5f);
					if (Up * Patch * Moss * 1.6f > 0.45f)
					{
						Col = FMath::Lerp(MossCol, MossLight, R.FRand());
						W *= 0.5f;
					}
				}
			}
			for (int32 K = 0; K < 3; K++)
			{
				M.C[3 * F + K] = Col;
				Wet[3 * F + K] = FVector2D(W, 0.0);
			}
		}
	}

	void Build(int32 Seed, const FAntsybalLook& Look, TArray<FVector2D>& Wet)
	{
		// Основа строится всегда с одним зерном — у всех анцыбалов одно тело.
		FRandomStream BaseStream(0x0A2C1BA1);
		M.R = &BaseStream;
		BuildBase();

		FRandomStream R(Seed);
		M.R = &R;
		const float Scale = RandIn(R, Look.Scale);
		const float Moss = RandIn(R, Look.Moss);
		AddDetails(Look);
		// Кочки мха — на загривке, плечах, макушке, сколько оброс.
		const TPair<FVector, FVector> MossSpots[] = {
			{ HumpTop, (Back + FVector::UpVector).GetSafeNormal() },
			{ Shoulder[0] + FVector(0, -2, 8), FVector(0, -0.3, 1).GetSafeNormal() },
			{ Shoulder[1] + FVector(0, 2, 8), FVector(0, 0.3, 1).GetSafeNormal() },
			{ HeadP(-2, 0, 11), FVector(-0.3, 0, 1).GetSafeNormal() },
			{ Pelvis + Spine * 30 + Back * 17, Back },
		};
		for (const TPair<FVector, FVector>& Spot : MossSpots)
			if (R.FRand() < Moss * 0.8f) MossClump(Spot.Key, Spot.Value, R.FRandRange(5.f, 9.f));
		Paint(Look, Moss, Wet);

		for (FVector& P : M.V) P *= Scale;
	}
};
}

AAntsybal::AAntsybal()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	// Тело для столкновений и попаданий появится вместе с поведением; меш — только картинка.
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Material = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Enemies/Antsybal/M_Antsybal.M_Antsybal")));
}

void AAntsybal::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// В игре облик всё равно выберется заново в BeginPlay — не строим дважды.
	if (!(bRandomSeedOnSpawn && GetWorld() && GetWorld()->IsGameWorld())) Build();
}

void AAntsybal::BeginPlay()
{
	Super::BeginPlay();
	if (bRandomSeedOnSpawn)
	{
		Seed = int32(HashCombine(FPlatformTime::Cycles(), GetUniqueID()));
		Build();
	}
	else if (Mesh->GetNumSections() == 0) Build();
}

void AAntsybal::Reroll()
{
	Modify();
	Seed = FMath::Rand();
	Build();
}

void AAntsybal::Build()
{
	FAntsybalBuilder B;
	TArray<FVector2D> Wet;
	B.Build(Seed, Look, Wet);
	Mesh->ClearAllMeshSections();
	Mesh->CreateMeshSection_LinearColor(0, B.M.V, B.M.T, B.M.N, Wet, B.M.C, TArray<FProcMeshTangent>(), false);
	if (UMaterialInterface* Mat = Material.LoadSynchronous()) Mesh->SetMaterial(0, Mat);
}

static FAutoConsoleCommandWithWorldAndArgs CmdAntsybalSpawn(
	TEXT("antsybal.Spawn"),
	TEXT("antsybal.Spawn [N=5] — N анцыбалов рядком перед игроком, у каждого свой облик."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
		const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!Pawn) return;
		const int32 Count = Args.Num() ? FMath::Clamp(FCString::Atoi(*Args[0]), 1, 32) : 5;
		const FVector Fwd = PC->GetControlRotation().Vector().GetSafeNormal2D();
		const FVector Right = FVector::UpVector ^ Fwd;
		FCollisionQueryParams Params;
		Params.AddIgnoredActor(Pawn);
		for (int32 I = 0; I < Count; I++)
		{
			FVector P = Pawn->GetActorLocation() + Fwd * 400 + Right * (I - (Count - 1) * 0.5f) * 150;
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 500), P - FVector(0, 0, 2000), ECC_Visibility, Params)) P = Hit.ImpactPoint;
			World->SpawnActor<AAntsybal>(P, (-Fwd).Rotation()); // мордой к игроку
		}
	}));
