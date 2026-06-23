#pragma once

#include "models/MediaPerson.h"

#include <QString>
#include <vector>

struct MediaItem {
    QString id;
    QString parentId;
    QString name;
    QString itemType;
    QString productionYear;
    QString seriesId;
    QString seriesName;
    QString seriesImageTag;
    QString seriesImageUrl;
    int childCount { 0 };
    QString overview;
    QString imageTag;
    QString imageUrl;
    QString backdropImageUrl;
    QString communityRating;
    QString officialRating;
    QString runTime;
    qint64 runTimeTicks { 0 };
    QString genres;
    QString people;
    std::vector<MediaPerson> peopleList;
    QString seasonName;
    QString indexNumber;
    QString parentIndexNumber;
    qint64 playbackPositionTicks { 0 };
    double playedPercentage { 0.0 };
    bool played { false };
    bool folder { false };
};
