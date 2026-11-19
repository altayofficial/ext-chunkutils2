--TEST--
Test SubChunk layer 0 and layer 1 handling
--SKIPIF--
<?php if(!extension_loaded("chunkutils2")) die("skip extension not loaded"); ?>
--FILE--
<?php

use pocketmine\world\format\PalettedBlockArray;
use pocketmine\world\format\SubChunk;

$layer0 = new PalettedBlockArray(0);
$layer0->set(0, 0, 0, 3);
$layer1 = new PalettedBlockArray(0);
$layer1->set(0, 0, 0, 8);

$sub = new SubChunk(0, $layer0, $layer1, new PalettedBlockArray(1));
var_dump($sub->getBlockLayer0() === $layer0, $sub->getBlockLayer1() === $layer1);
var_dump(count($sub->getBlockLayers()));
//block state accessors only look at layer 0
var_dump($sub->getBlockStateId(0, 0, 0));

//only layer 1 present
$only1 = new SubChunk(0, null, $layer1, new PalettedBlockArray(1));
var_dump($only1->getBlockLayer0(), $only1->isEmptyFast());
var_dump($only1->getBlockLayers()[0] === $layer1);
var_dump($only1->getBlockStateId(0, 0, 0));
var_dump($only1->getHighestBlockAt(0, 0));

//garbage collection moves a surviving layer 1 down into layer 0
$only1->collectGarbage();
var_dump($only1->getBlockLayer0() === $layer1, $only1->getBlockLayer1());
var_dump($only1->getBlockStateId(0, 0, 0));

//and drops layer 1 if it's empty
$emptyLayer1 = new SubChunk(0, $layer0, new PalettedBlockArray(0), new PalettedBlockArray(1));
$emptyLayer1->collectGarbage();
var_dump($emptyLayer1->getBlockLayer0() === $layer0, $emptyLayer1->getBlockLayer1());

//setBlockStateId creates layer 0 but leaves layer 1 alone
$fresh = new SubChunk(0, null, null, new PalettedBlockArray(1));
$fresh->setBlockStateId(1, 1, 1, 4);
var_dump($fresh->getBlockLayer0()?->get(1, 1, 1), $fresh->getBlockLayer1());

//clone and serialize keep both layers apart
$clone = clone $sub;
var_dump($clone->getBlockLayer0() !== $layer0, $clone->getBlockLayer1() !== $layer1);
var_dump($clone->getBlockLayer1()->get(0, 0, 0));
$restored = unserialize(serialize($only1));
var_dump($restored->getBlockLayer0()->get(0, 0, 0), $restored->getBlockLayer1());
$restored = unserialize(serialize($sub));
var_dump($restored->getBlockLayer0()->get(0, 0, 0), $restored->getBlockLayer1()->get(0, 0, 0));

//a cycle through a subclass property must be collectable
class CyclicSubChunk extends SubChunk{
	public ?SubChunk $self = null;
}
gc_collect_cycles();
for($i = 0; $i < 1000; $i++){
	$cyclic = new CyclicSubChunk(0, null, null, new PalettedBlockArray(1));
	$cyclic->self = $cyclic;
}
unset($cyclic);
var_dump(gc_collect_cycles() >= 1000);
?>
--EXPECT--
bool(true)
bool(true)
int(2)
int(3)
NULL
bool(false)
bool(true)
int(0)
NULL
bool(true)
NULL
int(8)
bool(true)
NULL
int(4)
NULL
bool(true)
bool(true)
int(8)
int(8)
NULL
int(3)
int(8)
bool(true)
