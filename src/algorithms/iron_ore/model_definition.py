"""Inference constants retained from the source project's accepted chemistry.

Extracted from training_physics/extended_physics.py and feo_extension/feo_chemistry.py
to avoid importing the training renderer and its network dependencies at prediction time.
"""
TARGETS = ('TFe', 'SiO2', 'Al2O3', 'TiO2', 'CaO', 'MgO', 'MnO', 'P', 'S', 'FeO')
MASS = dict(Fe=55.845, O=15.999, S=32.06)
FE_IN_FEO = MASS['Fe'] / (MASS['Fe'] + MASS['O'])
DEFINITION = 'FeO equivalent of ferrous iron in oxide/silicate phases, excluding pyrite iron; included in TFe'
