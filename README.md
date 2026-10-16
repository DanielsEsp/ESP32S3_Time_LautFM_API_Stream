# ESP32S3_Time_LautFM_API_Stream

Réalisation d'un streamer I2S composé d'un ESP32S3-N16R8 écran Waveshare 3,5' IPS capacitive touch.
Le code est largement développé avec l'aide de Claude_AI qui à décodé les datas de l'API LautFM,
n'ayant pas les compétences pour lire et décoder les différentes URLs et surtout à en extraire 
les images. 
L'utilisation de l'écran capacitif avec la gestion de 1 ou 2 doigts permet une navigation entre
les différentes pages.
Au démarrage l'horloge s'affiche, un toucher 1 doigt passe en mode radio avec la possibilité 
de sélectionner la station par Liste, Genre ou Favoris et de lancer la lecture avec les infos.
Lors de la lecture un appui 2 doigts repasse en mode sélection, un toucher 2 doigts sur cette page
stope la lecture et affiche à nouveau l'horloge.

<img width="1380" height="935" alt="radio_lecture" src="https://github.com/user-attachments/assets/b99c772f-b76c-42c1-9e61-6276fb1ddf11" />
<img width="1028" height="704" alt="radio_favorits" src="https://github.com/user-attachments/assets/9391ac44-b181-4613-9457-8a8d48250911" />
<img width="1083" height="688" alt="radio_genre" src="https://github.com/user-attachments/assets/c1f2bf2f-4516-4d40-9026-ba36068832ef" />
<img width="1067" height="704" alt="radio_liste" src="https://github.com/user-attachments/assets/ad769033-be34-46d1-9dea-598c9dbcd9e8" />
<img width="1226" height="792" alt="horloge" src="https://github.com/user-attachments/assets/a6d236da-c3ff-4522-b2e5-f9775daf21ba" />

Le montage permet le branchement de 1 ou 2 modules MAX98357A pour le raccordement 
à des HP de faible puissance ou un module PCM5102A pour piloter un entrée line d'ampli.

EVOLUTION, mode enregistrement Manuel et Auto-stop du stream et lecture des enregistrements.
AJOUT d'un ESP32 DevKit pour la sortie Bluetooth.

<img width="2453" height="2202" alt="LautFM_BT_JBL" src="https://github.com/user-attachments/assets/70de1305-973a-43a2-a949-1cbbeadc518f" />

