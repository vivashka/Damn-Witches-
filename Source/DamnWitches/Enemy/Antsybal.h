#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Antsybal.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

// Насколько разными бывают анцыбалы. Всё, что не здесь, — основа, одна на весь вид.
USTRUCT(BlueprintType)
struct FAntsybalLook
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere) FFloatInterval Scale = FFloatInterval(0.94f, 1.06f);
	// Насколько оброс: мох пятнами на гранях, глядящих вверх, и кочки мха на загривке, плечах, макушке.
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) FFloatInterval Moss = FFloatInterval(0.f, 1.f);
	// Насколько грязный и докуда (см от копыт) — где стоял в болоте.
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) FFloatInterval Mud = FFloatInterval(0.2f, 1.f);
	UPROPERTY(EditAnywhere) FFloatInterval MudLine = FFloatInterval(20.f, 80.f);
	// Мокрый — блестит (шероховатость в материале).
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) FFloatInterval Wetness = FFloatInterval(0.2f, 1.f);
	// Шерсть: густота и длина косм на ногах, тазу, хребте.
	UPROPERTY(EditAnywhere) FFloatInterval FurDensity = FFloatInterval(0.6f, 1.3f);
	UPROPERTY(EditAnywhere) FFloatInterval FurLength = FFloatInterval(0.8f, 1.3f);
	// Тина: волосы с затылка, борода, свисающая с плеч и рогов.
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) float HairChance = 0.75f;
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) float BeardChance = 0.75f;
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) float AlgaeChance = 0.5f;
	UPROPERTY(EditAnywhere) FFloatInterval HornLength = FFloatInterval(0.8f, 1.2f);
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) float BrokenHornChance = 0.15f;
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0)) int32 MaxWarts = 8;
	// Сдвиг тона кожи от основного к соседнему болотному.
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0, ClampMax = 1)) float SkinTint = 0.35f;
};

// Анцыбал — болотный чёрт (он же анчутка, болотник): видом как сатир — волосатый, рогатый, на козлиных ногах
// с копытами; у болотника вдобавок лягушачьи глаза навыкате, борода и волосы из тины, длинные руки, хвост.
//
// Облик собирается в три слоя:
//   1. Основа — тело, одно на весь вид (строится всегда с одним и тем же зерном). Отдаёт точки крепления.
//   2. Навесное — по точкам крепления из сида: рога, космы, тина, кочки мха, бородавки.
//   3. Поверхность — покраска граней из сида: грязь снизу, мох пятнами сверху, мокрота.
// Один меш, одна секция, один материал (цвет — в цвете вершин, мокрота — в UV0.x, альфа цвета — свечение глаз):
// один draw call на чёрта. Начало координат — под копытами, вперёд — +X.
UCLASS()
class DAMNWITCHES_API AAntsybal : public AActor
{
	GENERATED_BODY()
public:
	AAntsybal();

	UPROPERTY(EditAnywhere, Category = "Antsybal") int32 Seed = 1;
	// В игре облик выбирается заново при каждом появлении; в редакторе — по Seed.
	UPROPERTY(EditAnywhere, Category = "Antsybal") bool bRandomSeedOnSpawn = true;
	UPROPERTY(EditAnywhere, Category = "Antsybal") FAntsybalLook Look;
	UPROPERTY(EditAnywhere, Category = "Antsybal") TSoftObjectPtr<UMaterialInterface> Material;

	UFUNCTION(CallInEditor, Category = "Antsybal") void Build();
	UFUNCTION(CallInEditor, Category = "Antsybal") void Reroll();

	virtual void OnConstruction(const FTransform& Transform) override;

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere) TObjectPtr<UProceduralMeshComponent> Mesh;
};
