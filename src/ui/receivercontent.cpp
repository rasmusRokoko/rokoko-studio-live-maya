#include "receivercontent.h"
#include "receiverworker.h"
#include "animations.h"
#include "mapping.h"
#include "utils.h"

#include <QThread>
#include <QVBoxLayout>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QJsonArray>
#include <QTimer>
#include <QMenu>

#include <maya/MSceneMessage.h>
#include <maya/MFnDagNode.h>
#include <maya/MSelectionList.h>
#include <maya/MItSelectionList.h>
#include <maya/MGlobal.h>
#include <maya/MDagPath.h>


enum class RSObjectType : uint8_t {
    PROP = 0,
    TRACKER,
    FACE,
    ACTOR
};


ReceiverContent::ReceiverContent(QWidget* parent) : QWidget(parent)
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(1, 1, 1, 1);
    mainLayout->setSpacing(1);

    QFormLayout* formLayout = new QFormLayout(this);
    formLayout->setContentsMargins(1, 1, 1, 1);
    formLayout->setSpacing(1);

    // port field
    portBox = new QSpinBox(this);
    portBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    portBox->setAlignment(Qt::AlignCenter);
    portBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    portBox->setMaximum(65535);
    portBox->setValue(14043);

    formLayout->addRow("Port:", portBox);

    // scene scale field
    QDoubleSpinBox* sceneScaleBox = new QDoubleSpinBox(this);
    sceneScaleBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sceneScaleBox->setAlignment(Qt::AlignCenter);
    sceneScaleBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    sceneScaleBox->setRange(-100000, 100000);
    connect(sceneScaleBox, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged), [](double value) {
        Animations::get()->setSceneScale(value);
    });
    sceneScaleBox->setValue(100.0);
    formLayout->addRow("Scene scale:", sceneScaleBox);
    mainLayout->addLayout(formLayout);

    if(false)
    {
        // reset scene on stop option
        QCheckBox* cbResetSceneOnStop = new QCheckBox("Reset Scene on Stop", this);
        mainLayout->addWidget(cbResetSceneOnStop);

        // hide meshes during play
        QCheckBox* cbHideMeshesDuringPlay = new QCheckBox("Hide Meshes during Play", this);
        mainLayout->addWidget(cbHideMeshesDuringPlay);
    }

    // start receiver button
    ButtonParams receiverBtnParams;
    receiverBtnParams.iconSize = 16;
    receiverBtnParams.checkedIconPath = ":/resources/pause.svg";
    receiverBtnParams.uncheckedIconPath = ":/resources/play.svg";
    receiverBtnParams.checkedStateText = "Stop Receiver";
    receiverBtnParams.uncheckedStateText = "Start Receiver";
    receiverBtnParams.iconAlignment = Qt::AlignLeft;

    startReceiverBtn = new Button(this, receiverBtnParams);
    startReceiverBtn->setCheckable(true);
    mainLayout->addWidget(startReceiverBtn);
    connect(startReceiverBtn, &Button::toggled, this, &ReceiverContent::onReceiverToggled);

    // record button
    startRecordingBtn = new RecordButton(this);
    startRecordingBtn->setEnabled(false);
    mainLayout->addWidget(startRecordingBtn);
    connect(startReceiverBtn, &QPushButton::toggled, startRecordingBtn, &QPushButton::setEnabled);
    connect(startRecordingBtn, &QPushButton::toggled, this, &ReceiverContent::recordingToggled);

    // error label
    statusLabel = new QLabel("", this);
    mainLayout->addWidget(statusLabel);

    // tree widget
    treeWidget = new QTreeWidget(this);
    treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(treeWidget, &QTreeWidget::customContextMenuRequested, this, &ReceiverContent::prepareContextMenu);
    treeWidget->headerItem()->setHidden(true);
    treeWidget->setRootIsDecorated(false);
    mainLayout->addWidget(treeWidget);

    // create worker and move it to thread
    worker = new DataReceivingWorker(this);
    connect(worker, &DataReceivingWorker::workerStateChanged, [&](QString status){
        statusLabel->setText(status);
    });
    connect(worker, &DataReceivingWorker::onSocketConnected, this, &ReceiverContent::populateTree);

    // register scene callbacks
    // register callbacks
    MCallbackId beforeNewId = MSceneMessage::addCheckCallback(MSceneMessage::kBeforeNewCheck, [](bool* recCode, void* clientData) {
        ReceiverContent* receiverWidget = static_cast<ReceiverContent*>(clientData);
        if(receiverWidget) {
            receiverWidget->reset();
        }
        *recCode = true;
    }, this);
    MCallbackId beforeOpenId = MSceneMessage::addCheckCallback(MSceneMessage::kBeforeOpenCheck, [](bool* recCode, void* clientData) {
        ReceiverContent* receiverWidget = static_cast<ReceiverContent*>(clientData);
        if(receiverWidget) {
            receiverWidget->reset();
        }
        *recCode = true;
    }, this);
    callbacks.append(beforeNewId);
    callbacks.append(beforeOpenId);

    Animations::get()->setStopReceiverCallback(std::bind(&ReceiverContent::reset, this));
}

ReceiverContent::~ReceiverContent()
{
    worker->pause();
    for(MCallbackId id : callbacks) {
        MSceneMessage::removeCallback(id);
    }
    callbacks.clear();
}


void ReceiverContent::onReceiverToggled(bool checked)
{
    if(checked)
    {
        worker->start(portBox->value());
    } else {
        reset();
    }
}

void ReceiverContent::prepareContextMenu(const QPoint &pos)
{
    QTreeWidgetItem* item = treeWidget->itemAt(pos);
    if(item) {
        QString itemlabel = item->text(0);
        QString itemId = item->data(0, Qt::UserRole).toString();
        RSObjectType itemType = static_cast<RSObjectType>(item->data(1, Qt::UserRole).toInt());

        // this item can't be mapped
        if(itemId.isEmpty()) return;

        // check item type and create special menus for each type

        QMenu menu(this);
        if(itemType == RSObjectType::PROP || itemType == RSObjectType::TRACKER) {
            menu.addAction("Map to selected objects", [=](){
                Mapping::get()->mapRSObjectToSelection(itemId);
            });
            menu.addAction("Unmap selected objects", [=](){
                Mapping::get()->unmapRSObject(itemId, true);
            });
            menu.addAction("Unmap all", [=](){
                Mapping::get()->unmapRSObject(itemId, false);
            });
            menu.addAction("Select objects", [=](){
                Mapping::get()->selectObjects(itemId);
            });
        }

        if(itemType == RSObjectType::ACTOR)
        {
            menu.addAction("Create HIK skeleton", [=](){
                Mapping::get()->createHIKForActor(itemId);
            });

            menu.addAction("Map to active character", [=](){
                Mapping::get()->mapActorToCurrentMayaCharacter(itemId);
            });

            menu.addAction("Unmap character", [&](){
                QString currentChar = treeWidget->currentItem()->text(0);
                // check if more than 1 character mapped and prompt dialog with selection
                auto objectMapping = Mapping::get()->getObjectMapping();
                int count = objectMapping.count(currentChar);
                if (count > 1) {
                    auto it = objectMapping.find(currentChar);
                    QStringList mappedHipJoints;
                    // populate mapped character root bones
                    while(it != objectMapping.end() && it.key() == currentChar) {

                        MObject object; MDagPath path; MStatus result;
                        Utils::getObjectFromString(it.value(), object, path, result);

                        MFnDagNode node(object);
                        QString temp(node.name().asChar());
                        mappedHipJoints << temp.split("_").takeFirst();
                        ++it;
                    }
                    QString selectedCharacter = QInputDialog::getItem(this, "Select character", "Select character to unmap", mappedHipJoints, 0, false);
                    Mapping::get()->unmapMayaObjectByName(QString("%1_Hips").arg(selectedCharacter));
                } else {
                    Mapping::get()->unmapRSObject(itemId, false);
                }
             });

            menu.addAction("Unmap all", [=](){
                Mapping::get()->unmapRSObject(itemId, false);
            });

            menu.addAction("Select mapped skeletons", [=](){
                Mapping::get()->selectObjects(itemId);
            });
        }

        if(itemType == RSObjectType::FACE) {
            menu.addAction("Map to selected objects", [=](){
                MSelectionList ls;
                MGlobal::getActiveSelectionList(ls);
                if(ls.length() > 0)
                {
                    MItSelectionList it(ls);
                    while(!it.isDone()) {
                        MDagPath objPath;
                        it.getDagPath(objPath);
                        Mapping::get()->mapFaceToMayaObject(objPath.fullPathName().asChar(), itemId);

                        it.next();
                    }

                }
            });
            menu.addAction("Unmap selected objects", [=](){
                MSelectionList ls;
                MGlobal::getActiveSelectionList(ls);
                if(ls.length() > 0)
                {
                    MItSelectionList it(ls);
                    while(!it.isDone()) {
                        MDagPath objPath;
                        it.getDagPath(objPath);
                        Mapping::get()->unmapFaceFromMayaObject(objPath.fullPathName().asChar());
                        it.next();
                    }
                }
            });

            menu.addAction("Unmap all", [=](){
                Mapping::get()->unmapAllFaces(itemId);
            });

            menu.addAction("Automap weights", [=](){
                Mapping::get()->automapWeights(itemId);
            });

        }

        menu.exec(treeWidget->mapToGlobal(pos));
    }

}

void ReceiverContent::recordingToggled(bool checked)
{
    Animations::get()->recordingToggled(checked);
}

void ReceiverContent::populateTree()
{
    QTimer::singleShot(250, [&](){
        // populate props
        treeWidget->clear();
        auto props = Animations::get()->getProps();
        QHash<QString, QTreeWidgetItem*> propsItemMap;
        foreach(auto prop, props) {
            QTreeWidgetItem* propItem = new QTreeWidgetItem(treeWidget);
            propItem->setText(0, prop["name"].toString());
            propItem->setIcon(0, QIcon(":/resources/cube.png"));
            // put rs object id into user data, to e used by context menu
            propItem->setData(0, Qt::UserRole, QVariant(prop["id"].toString()));
            propItem->setData(1, Qt::UserRole, QVariant((int)RSObjectType::PROP));
            propsItemMap[prop["id"].toString()] = propItem;
        }

        // actor profiles
        QHash<QString, QTreeWidgetItem*> profilesMap;
        QHash<QString, QTreeWidgetItem*> actorsMap;
        auto actors = Animations::get()->getActors();
        foreach(auto actor, actors) {
            QTreeWidgetItem* profileItem = new QTreeWidgetItem(treeWidget);
            QString profileName = actor["profileName"].toString();
            QString actorName = actor["name"].toString();
            profileItem->setText(0, profileName);
            profileItem->setIcon(0, QIcon(":/resources/icon-rokoko-32.png"));
            profilesMap[profileName] = profileItem;
            QTreeWidgetItem* actorItem = new QTreeWidgetItem(profileItem);
            actorItem->setText(0, actorName);
            actorItem->setIcon(0, QIcon(":/resources/icon-row-suit-32.png"));
            // put rs object id into user data, to e used by context menu
            actorItem->setData(0, Qt::UserRole, QVariant(actor["id"].toString()));
            actorItem->setData(1, Qt::UserRole, QVariant((int)RSObjectType::ACTOR));

            actorsMap[actor["id"].toString()] = actorItem;
        }

        // populate faces
        auto faces = Animations::get()->getFaces();
        foreach(auto face, faces) {
            QTreeWidgetItem* faceItem = nullptr;
            if(face.contains("profileName")) {
                QString faceProfileName = face["profileName"].toString();
                if(profilesMap.contains(faceProfileName))
                    faceItem = new QTreeWidgetItem(profilesMap[faceProfileName]);
            } else
                faceItem = new QTreeWidgetItem(treeWidget);

            QString faceId = face["faceId"].toString();
            faceItem->setText(0, faceId);
            faceItem->setIcon(0, QIcon(":/resources/icon-row-face-32.png"));
            faceItem->setData(0, Qt::UserRole, QVariant(faceId));
            faceItem->setData(1, Qt::UserRole, QVariant((int)RSObjectType::FACE));
        }

        // populate trackers
        auto trackers = Animations::get()->getTrackers();
        foreach(auto tracker, trackers) {
            QString propParentId = tracker["connectionId"].toString();
            QTreeWidgetItem* parentItem = nullptr;
            if(propsItemMap.contains(propParentId))
                parentItem = propsItemMap[propParentId];
            else if(actorsMap.contains(propParentId))
                parentItem = actorsMap[propParentId];

            QTreeWidgetItem* trackerItem = new QTreeWidgetItem(parentItem);
            QString trackerId = tracker["name"].toString();
            trackerItem->setText(0, trackerId);
            trackerItem->setIcon(0, QIcon(":/resources/icon-vp-32.png"));
            trackerItem->setData(0, Qt::UserRole, QVariant(trackerId));
            trackerItem->setData(1, Qt::UserRole, QVariant((int)RSObjectType::TRACKER));
        }

        treeWidget->expandAll();
        treeWidget->setItemsExpandable(false);

        Mapping::get()->syncMapping();
    });
}

void ReceiverContent::clearTreeWidget()
{
    treeWidget->clear();
}

void ReceiverContent::reset()
{
    worker->pause();
    clearTreeWidget();
    statusLabel->setText("");
    startReceiverBtn->setChecked(false);

}

